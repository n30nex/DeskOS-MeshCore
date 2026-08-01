HISTORICAL RECORD — DO NOT EXECUTE

This document predates the RC1 authority reset. It is retained only for
provenance and cannot create work, tests, evidence requirements, or release
gates. See `docs/RC1_SCOPE.md` and `docs/ROADMAP.md`.

# SIGUI / MeshCore DeskOS D1L
## Full Repository Audit and Master Completion Roadmap

> **SUPERSEDED HISTORICAL ROADMAP — NOT CURRENT RC1 RELEASE AUTHORITY.**
> This audit preserves its dated dependency/evidence analysis, but its
> capability state, COM routing, SD/NVS policy and soak finish line are not
> current instructions. Use the [project README](../../README.md),
> [RC1 user guide](../USER_GUIDE_D1L.md),
> [RC1 test plan](../TEST_PLAN_D1L.md), and
> [SD-card guide](../D1L_SD_CARD_GUIDED_INSTALL.md).

**Repository:** `n30nex/SIGUI`  
**Audit date:** 2026-07-12  
**Prepared for:** Codex 5.6 Ultra and the SIGUI maintainers  
**Merged baseline audited:** `main` at `5b5dfaa0592347497df1a2f77f572c6d49933c6a`  
**Newest integrated candidate audited:** `07322ed4c700866106ecca6c31ff70ea3a3d4ede`  
**Active pull-request stack:** PR #62 → PR #64 → PR #80  
**Pinned MeshCore reference:** `e8d3c53ba1ea863937081cd0caad759b832f3028`  
**Selected SDK:** ESP-IDF `v5.5.4`

> This document supersedes the older broad roadmap as the execution source of truth. It does not erase the detailed evidence already recorded in `docs/ROADMAP.md`, `docs/KRABSTHOUGHTS.MD`, issue threads, CI artifacts, or hardware receipts. It converts that evidence into a dependency-ordered completion program with explicit stop conditions.

**Live reconciliation, 2026-07-17:** exact main through PR #182 is `de0bb75bd91146f0dc9896540d12c71889d7766b`. Exact-main Actions `29548300732` pass 1,263 host, 33 checksum, 1,008 wire-vector, 931 oracle, and 100,000 native plus 100,000 Clang 18 semantic-packet cases with zero findings. All five API ZIPs and 46 nested checksum entries verify across 341 files / 27,208,527 archive bytes with exact-source provenance and SPDX 2.3; canonical strict receipt SHA-256 is `8da06d90df77a439e37892560272f902243776107365a1676fdd5a49824b74d9`, and gate-manifest SHA-256 is `11c349cf8ad118463db411d7cf83366cdf86b5b67cbcc20034be845693c03ede`. The WP-05 matrix is 13 implemented / 3 partial / 0 missing semantic requirements with 11 suites / 75 scenarios; fuzz is 4 implemented / 1 partial / 3 missing. PRs #171/#173 bank Admin-owner and bounded queue-fairness foundations, but WP-06 remains incomplete. A non-erasing exact COM12 flash passed; retained predecessor DM data is preserved behind an explicit compatibility/migration blocker, while SD/media health remained green. Official-peer RF, physical acceptance, full WP-05/WP-06, and release closure remain open. The strict audit remains fail-closed with 33 P0 / 35 total failures; capability reporting remains 80.3703703704% raw / 80%, weighted progress 74.2962962963% raw / 74%, and final gates 0 of 11 green.

---

## 1. Executive verdict

SIGUI is **not yet release-ready**, but it is no longer an early prototype. It has a substantial, testable platform: a working D1L board layer, display and touch shell, SX1262 radio path, Public messaging, partial direct messaging, contact/node/route/packet models, non-destructive FAT32 SD support through the RP2040, Wi-Fi, a bounded current-view map, diagnostics, a host simulator, hundreds of contract tests, packaged GitHub Actions artifacts, checksums, and unusually strong hardware-evidence tooling.

The project is blocked by five classes of problems:

1. **The active code is stranded in a three-level draft PR stack.** The latest integrated candidate is 30 commits ahead of `main`. Its green CI does not make `main` production-ready, and merging the stack changes the final release SHA and invalidates “exact-head” evidence unless the final merged result is requalified.
2. **The newest retained-storage and SD-removal work is not physically closed.** Predecessor heads produced a route-persistence stack overflow, a post-ack watchdog reset, and a false inserted-card `no_card` state. The current pair contains a targeted repair, but it still needs exact-artifact repeated inserted-card and physical remove/reinsert proof.
3. **MeshCore protocol compatibility is incomplete.** Current CI proves only the structural wire envelope. SIGUI locally reimplements most packet/session behavior while compiling only the upstream Ed25519 C sources. Production cryptography, signed advert semantics, DM ACK/PATH, retries, route fallback, duplicate/replay handling, channels, trace, and authenticated server sessions are not yet completely proven against the pinned upstream implementation.
4. **Runtime ownership is too diffuse.** Radio callbacks perform parsing, storage updates, flushes, and status mutation. The UI and USB console are large monoliths. This makes race conditions, stack pressure, lifetime bugs, and regressions more likely precisely when the remaining features are added.
5. **Several requested “complete firmware” capabilities remain absent.** BLE companion transport, release-grade update/recovery, authenticated repeater/room administration, complete multi-channel support, truthful DM delivery, fully real trace/PATH behavior, notification polish, a production terminal/log view, and final end-to-end release evidence are unfinished.

The fastest safe route is **not** another broad rewrite. It is:

1. freeze and physically close the current PR #80 candidate;
2. land the stacked work into `main` in order;
3. establish one protocol/runtime owner and a pinned upstream conformance oracle;
4. complete DM, contacts/channels, routing/trace, persistence/time, and admin semantics behind stable application contracts;
5. extract UI screens incrementally around those contracts;
6. add the remaining full-feature train—BLE, notification/emoji polish, terminal, and signed update/recovery;
7. run one final exact-commit hardware/RF/SD/soak/release gate and tag only that commit.

**Completion must mean an exact tagged commit, reproducible Actions package, complete evidence bundle, zero known P0/P1 release defects, and no unsupported UI feature presented as working.**

---

## 2. Audit scope, method, and confidence

### 2.1 Material inspected

The audit covered the current repository state and active integration line, including:

- repository metadata, branches, commit history, open issues, pull requests, and workflow status;
- `main` and the newest combined PR #80 head;
- top-level build and partition configuration;
- ESP-IDF, Seeed BSP, and submodule integration;
- application initialization and fail-closed behavior;
- SX1262/MeshCore service, packet envelope, messaging, identity, contacts, nodes, routes, packet log, read state, and retained storage;
- RP2040 UART/SD bridge, bridge firmware, storage manager, file protocol, map cache, export paths, and reboot/quiescence;
- Wi-Fi runtime and current BLE build state;
- LVGL UI shell, major screens, map service, simulator and capture contracts;
- USB JSON console and hardware automation;
- release-gate, soak, UI, SD, RF, checksum, and packaging scripts;
- current project docs and the issue-level release plan;
- pinned upstream MeshCore `BaseChatMesh` behavior for ACK, PATH, attempts, channels, contacts, and authenticated requests;
- official OpenStreetMap tile-use policy, GitHub Actions hardening guidance, and the selected ESP-IDF release documentation.

### 2.2 Important limitation

This was a source, history, issue, CI, and evidence audit. The auditor did **not** independently flash the physical D1L or run a new RF exchange. Existing hardware conclusions are therefore accepted only to the extent supported by commit-matched repository receipts and PR descriptions. The roadmap deliberately requires fresh exact-head proof wherever existing evidence is stale, incomplete, or tied to a superseded SHA.

### 2.3 Confidence levels used

- **Confirmed:** directly visible in current source, issue state, workflow state, or commit-matched evidence.
- **Strong inference:** follows from source architecture or configuration and must be validated during implementation.
- **Open:** requires physical hardware, RF peer, credentials, or a product decision.

---

## 3. Non-negotiable project constraints

These constraints are mandatory for every work package and must be encoded in scripts, CI, documentation, and the Codex operating prompt.

1. **Firmware builds happen only in GitHub Actions.** Local work may run host tests, static analysis, simulators, and artifact verification. Do not run local ESP-IDF or RP2040 release builds as substitute evidence.
2. **Flash only downloaded Actions artifacts with verified checksums.**
3. **Never use COM8, COM11, or COM29 for D1L/RP2040/peer automation.** The current repo’s COM11 helper default must be removed. Require explicit `D1L_PORT`, `RP2040_PORT`, and `MESH_PEER_PORT`; fail closed when a forbidden port is supplied.
4. **Do not format SD cards on-device.** Users provide FAT32 media. Firmware may detect, mount, create the DeskOS directory structure, validate, and use it. It must preserve existing data and give repair guidance for unusable cards.
5. **No Public RF during storage, UI, Wi-Fi, map, build, or general diagnostics.** RF tests use `#test` or targeted DMs only.
6. **Do not retest RF for unrelated changes.** Run the smallest issue-specific hardware gate; run the full RF matrix only after radio/protocol/session changes and for the final RC.
7. **Default radio profile remains Canada/USA:** 910.525 MHz, 62.5 kHz, SF7, CR5, 20 dBm, TCXO NONE, unless an explicit user setting overrides it.
8. **The D1L remains a non-forwarding standalone client.** Do not imply repeater or room-server behavior merely because those remote roles can be viewed or administered.
9. **Maximum user message input is 138 UTF-8 bytes unless the pinned upstream protocol proves a different lower limit.** The UI counter must distinguish visible characters from encoded bytes.
10. **No feature may be displayed as active when its runtime is absent.** This currently applies especially to BLE, observer mode, OTA, local GPS, and remote admin.
11. **Preserve existing identity, settings, retained history, and user SD data across normal upgrades.** Destructive recovery requires an explicit, narrowly scoped, operator-approved procedure and evidence receipt.
12. **Every physical claim must name the exact commit, Actions run, package checksum, device port, RP2040 peer artifact where applicable, reset reason, and artifact path.**
13. **Every production source change must be tied to an issue/work package, tests, documentation, and a bounded acceptance artifact.**
14. **Do not stop simply because one physical gate is blocked.** Record the exact external blocker, continue every unblocked host/source/documentation slice, and return to the gate when the required hardware input exists.

---

## 4. Current repository and integration state

### 4.1 Branch and PR topology

| Line | State | Key observation |
|---|---|---|
| `main` at `5b5dfaa` | Merged baseline | Stable documentation/storage hierarchy baseline, but missing the large Map/SDK/durability stack. |
| PR #62 `codex/d1l-ui-map-hierarchy` | Open draft, base `main` | Built-in bounded OpenStreetMap current-view implementation and Map UI hierarchy. |
| PR #64 `codex/d1l-idf55-migration` | Open draft, base PR #62 | ESP-IDF 5.5.4, dependency lock, BSP compatibility, Wi-Fi/Map integration, memory policy, and broader platform work. |
| PR #80 latest head `07322ed` | Open draft, base PR #64 | MeshCore envelope conformance, retained-store durability, release-proof tightening, and current SD false-`no_card` repair candidate. |
| `feature/meshcore-deskos-d1l` | Diverged/stale | Do not merge wholesale. Verify unique commits, then archive after the active stack lands. |

The current head is cleanly ahead of `main`, but the three stacked PRs are too large for continued feature development. No new broad feature work should be stacked on PR #80. First close its exact hardware gate and land the stack.

### 4.2 Current CI state

The newest PR #80 head has green jobs for:

- host test suite;
- MeshCore wire-envelope sanitizer/fuzz job;
- RP2040 SD bridge and official Seeed smoke builds;
- ESP32 firmware build on ESP-IDF 5.5.4;
- package assembly;
- checksum verification;
- dry-run release, UI, SD, and RF tooling.

The PR reports **614 passing host tests**. This is a strong regression net, but the conformance artifact explicitly says `coverage_boundary=wire_envelope_only` and `closure_ready=false`. Green CI is therefore necessary but not sufficient.

### 4.3 Current hardware risk on the active line

The retained-storage sequence exposed three separate defects on predecessor heads:

- route persistence caused a task stack-overflow panic;
- a controlled reboot sequence later produced a watchdog reset;
- a subsequent route write saw a false RP2040 `no_card` result while the card remained inserted and later recovered to `READY_SD`.

The newest ESP32/RP2040 pair attempts to distinguish true removal from suspect GPIO7 card-detect samples by performing a bounded, read-only sector-zero CMD17 transaction and validating token/CRC. This repair is plausible and appropriately narrow, but it is **not accepted until exact-pair physical proof passes**.

### 4.4 Code-size and change-risk hotspots

The following hotspots are not automatically “bad,” but they concentrate unrelated responsibilities and are the most likely source of release regressions:

| File | Approximate size | Risk |
|---|---:|---|
| `main/ui/ui_phase1.c` | 8,544 lines | Global LVGL state, many screens/modals/callback lifetimes, mixed navigation/rendering/actions. |
| `main/comms/usb_console.c` | 5,124 lines | Parsing, command dispatch, JSON rendering, hardware control, diagnostics, test hooks. |
| `scripts/release_gate_audit_d1l.py` | 3,430 lines | Large evidence policy engine with current forbidden-port default drift. |
| `firmware/rp2040_sd_bridge/...ino` | 3,093 lines | SD electrical probing, mount, file protocol, state/cache/removal behavior. |
| `main/mesh/meshcore_service.c` | 1,826 lines | Radio ownership, local protocol codec, crypto/session behavior, callbacks, store updates. |
| `main/storage/retained_blob_store.c` | 1,502 lines | NVS ownership/migration, SD generation, mirror policy, failure telemetry. |
| `main/hal/rp2040_bridge.c` | 1,389 lines | UART protocol, locking, quiescence, file/status/diag commands. |
| `main/storage/storage_status.c` | 1,276 lines | Storage state machine, recovery/reset, backend transitions. |

The completion roadmap uses incremental extraction and contract tests—not a big-bang rewrite.

---

## 5. Architecture as implemented today

### 5.1 Boot path

`app_main()` initializes default NVS, dedicated retained NVS ownership, health monitoring, storage status, RP2040 bridge and SD boot preparation, crash log, settings, each retained model, persistence worker, storage manager, MeshCore service, board, radio RX, connectivity, UI, and finally the blocking USB console.

Positive properties:

- NVS and retained NVS errors fail closed rather than broadly erasing user data.
- SD and RP2040 failures do not block a usable NVS fallback.
- board/display failure leaves a serial diagnostic path.
- radio RX is started only after board initialization.
- the console remains available for evidence collection.

Risks:

- initialization success is logged but there is no single explicit application capability registry; the UI must infer readiness across many globals;
- partial subsystem failure can leave a nominally running UI with ambiguous feature availability;
- several long-lived workers and stores initialize independently, increasing shutdown/reboot coordination complexity.

**Target:** produce one immutable `d1l_capabilities_snapshot_t` and one health state after boot, update it through owned events, and render all feature availability from that source.

### 5.2 Radio and MeshCore path

SIGUI compiles selected upstream Ed25519 C sources but locally implements packet envelope, encryption/decryption glue, adverts, Public messages, DMs, ACK parsing, route observation, trace token behavior, and service state.

Current service queue commands are essentially “start RX” and “send raw.” Radio callbacks can perform substantial work directly: parse packets, mutate status, update stores, and request persistence. The queue is small and async callback error handling is limited.

**Target:** the radio callback does only bounded capture/validation and enqueues an immutable event. One Mesh runtime task owns all protocol/session/contact/route state. UI, console, storage, and diagnostics communicate through commands and snapshots.

### 5.3 Storage path

The ESP32 owns policy and sends file/status operations over UART to the RP2040, which owns the physical microSD interface. The project now has:

- FAT32-only, no-format policy;
- directory/manifest creation;
- file read/write/delete/list/rename;
- atomic replace strategy;
- SD/NVS backend generations;
- dedicated retained NVS partition and ownership markers;
- compact NVS fallback;
- coalesced route persistence and later similar behavior for other stores;
- exports and map cache;
- detailed status and fault telemetry.

This is a strong design, but it has become a distributed transaction system across two MCUs, removable media, four retained models, two NVS partitions, and reboot quiescence. It needs explicit invariants, fault-injection tests, and one generic persistence state machine instead of store-specific drift.

### 5.4 UI path

The LVGL shell is functional and has simulator/capture contracts. It includes Home, Messages, Network, Map, More, nested detail pages, compose keyboard, contacts, roles, storage, Wi-Fi, settings, packet views, diagnostics, and numerous modals.

The current monolith couples:

- widget creation and lifetime;
- navigation;
- model queries;
- command execution;
- periodic refresh;
- map frame ownership;
- serial-test hooks;
- styling;
- keyboard and modal state.

**Target:** retain the visible design, but extract screen controllers one at a time. LVGL remains owned by one UI task. Screen controllers consume immutable view models and emit application commands. No screen calls radio/storage implementation directly.

### 5.5 Connectivity and Map path

Wi-Fi uses ESP-IDF station mode, saved credentials, scan, status, and TLS/SNTP support. BLE is disabled in the production configuration and returns only a placeholder state.

The Map service is thoughtfully bounded:

- only a user-opened current viewport;
- a maximum visible 3×3 tile set at one zoom;
- sequential HTTPS requests;
- explicit User-Agent;
- TLS certificate validation after SNTP;
- PSRAM double buffering;
- cancellation through generation/visibility;
- 429/503 backoff;
- PNG/type/size validation;
- atomic cache writes and attribution metadata;
- persistent visible attribution;
- no area or multi-zoom prefetch.

Open work includes physical exact-head proof, map marker integration, cache lifecycle/provider abstraction, and robust Wi-Fi reconnect behavior.

---

## 6. What is already strong and should be preserved

1. **Fail-closed, evidence-driven release culture.**
2. **GitHub Actions-only firmware packaging and checksum flow.**
3. **Exact-commit hardware artifact naming and JSON receipts.**
4. **Simulator screenshots and physical pixel-diff tooling.**
5. **Non-destructive SD policy and NVS fallback.**
6. **Dedicated retained partition and explicit ownership markers.**
7. **Bounded Map network/storage behavior.**
8. **Persistent identity and Canada/USA radio defaults.**
9. **Structured JSON serial console for automation.**
10. **Host tests for UI contracts, storage policy, release evidence, and packet envelope fuzzing.**
11. **Explicit separation between “green build” and “release closure.”**
12. **Issue-level statements of proof and constraints.**

The roadmap must build on these assets rather than replacing them.

---

## 7. Critical and high-severity findings

### F-01 — The production line is not on `main`  
**Severity:** Critical  
**Evidence:** PR #62 → #64 → #80; newest head 30 commits ahead of `main`.

**Impact:** New development on top of the stack increases conflict, review, regression, and evidence invalidation. A release from a draft stack is difficult to reproduce and maintain.

**Required action:** close the exact PR #80 hardware gate, merge the stack in order, rerun Actions after each retarget, then run a minimal exact-merged-main hardware gate.

---

### F-02 — Current SD false-`no_card` repair is unqualified  
**Severity:** Critical  
**Evidence:** prior exact heads produced PANIC, WDT, and false absent-card behavior; current pair is untested on the full required physical cycle.

**Impact:** retained route/message/packet writes can fail or switch backend unexpectedly with an inserted card; repeated recovery could damage UX or expose more reboot races.

**Required action:** run the dedicated WP-01 matrix before merging. No other feature work may change RP2040/storage code until this result is banked.

---

### F-03 — Wire-envelope conformance is not protocol conformance  
**Severity:** Critical  
**Evidence:** current artifact explicitly covers only header/path/payload framing. `main/CMakeLists.txt` compiles upstream Ed25519 C files, not the upstream Mesh, BaseChatMesh, dispatcher, contacts/channels, or session implementation.

**Impact:** a packet can be structurally well-formed while cryptographically or semantically incompatible with official MeshCore peers.

**Required action:** compile a host-side pinned-upstream oracle and compare bidirectional vectors for every declared 1.0 feature.

---

### F-04 — Inbound DMs do not transmit the required ACK/PATH response  
**Severity:** Critical  
**Evidence:** current inbound DM path decrypts, stores, computes an ACK hash, but does not send it.

**Impact:** official peers cannot reach truthful delivery confirmation, may retry unnecessarily, and cannot learn a return path correctly.

**Required action:** implement upstream-compatible direct ACK, flood ACK+PATH, timing, duplicate handling, and bounded re-ACK behavior.

---

### F-05 — Outbound DM “sent” state is not truthful  
**Severity:** Critical  
**Evidence:** local history is appended after a raw transmit request is accepted rather than after radio completion/ACK; attempt is effectively fixed; expected-ACK sessions and retries are incomplete.

**Impact:** UI can report success when the radio timed out, recipient was absent, ACK was lost, queue was saturated, or device rebooted.

**Required action:** introduce an explicit persisted DM delivery state machine and make the UI reflect it.

---

### F-06 — Trace is currently a text-message token, not a real route trace  
**Severity:** Critical for advertised trace feature  
**Impact:** route diagnostics can appear meaningful while not exercising the protocol’s true PATH/trace behavior.

**Required action:** implement the pinned upstream trace/path-discovery semantics, response correlation, hop rendering, timeout, and privacy boundaries. Keep the old token probe only as a test helper and label it as such.

---

### F-07 — Heard nodes and canonical contacts are not the same thing  
**Severity:** High  
**Impact:** a fingerprint-only heard node may not have a verified full public key, shared secret, role, or sendable contact record. Presenting “Message” too early creates dead-end or unsafe DM flows.

**Required action:** define one canonical identity/contact model. Enable DM only after signed advert/import/promotion yields a verified full key and compatible role.

---

### F-08 — Multi-channel support is not complete  
**Severity:** High  
**Evidence:** current service has one hard-coded Public secret/hash. Settings schema has no complete channel collection.

**Impact:** the UI and data model cannot truthfully claim MeshCore channel parity; future channel work risks duplicating state across settings, UI, and protocol.

**Required action:** add bounded channel slots, names, secrets, enable/default state, import/export, unread/read state, per-channel history, and conformance vectors.

---

### F-09 — Radio callbacks and global state violate single-owner runtime discipline  
**Severity:** Critical stability risk  
**Impact:** callback stack use, store flushes, direct status mutation, and cross-task access create races and contributed to stack/reboot fragility.

**Required action:** callbacks enqueue; one runtime task owns protocol state. All snapshots are copied under a bounded lock or published atomically.

---

### F-10 — Persistence logic is powerful but duplicated and difficult to reason about  
**Severity:** High  
**Impact:** Public, DM, route, and packet stores can drift in dirty/epoch/lineage/generation/reconcile behavior. Hot-path writes can cause flash wear and timing stalls.

**Required action:** freeze current proven behavior, formalize invariants, then factor shared scheduling/reconciliation primitives behind exhaustive native tests.

---

### F-11 — Unknown/future settings schemas can be treated too aggressively  
**Severity:** High  
**Impact:** a downgrade or newer schema can be mistaken for corruption/defaults, risking silent loss of identity, Wi-Fi, radio, map, or future settings.

**Required action:** add checksummed envelopes, explicit `schema_newer_than_firmware` quarantine, migration journal, rollback-safe writes, and scoped reset domains.

---

### F-12 — Time is monotonic but not yet truthful wall-clock time  
**Severity:** High for UI and replay semantics  
**Impact:** timestamps may be unique enough for packet generation but not reliable for user-visible time, ordering across resets, expiry, or admin sessions.

**Required action:** create a time service with separate monotonic protocol clock and validated wall clock. Expose validity/source; never display fabricated local time.

---

### F-13 — Wi-Fi has no complete bounded reconnect policy  
**Severity:** High  
**Evidence:** disconnect events clear state; current code does not expose a full exponential reconnect state machine. Startup failure can disable Wi-Fi to fail closed.

**Impact:** transient AP loss can leave the Map unusable until manual intervention; broad disable-on-error can be surprising.

**Required action:** implement bounded retry/backoff, explicit auth/no-AP errors, safe-mode crash counter, user cancel, and memory/stack telemetry.

---

### F-14 — The current UI and console are change-risk multipliers  
**Severity:** High  
**Impact:** remaining protocol, admin, BLE, update, and notification work will make the monolith harder to test and more likely to crash.

**Required action:** extract incrementally with compatibility wrappers. No all-at-once UI rewrite.

---

### F-15 — Authenticated repeater/room administration is partial
**Severity:** Critical against the current 1.0 product contract  
**Impact:** PR #166 supplies bounded read-only repeater login/status behavior with raw Admin TX service-queued but Admin runtime/session mutation still caller-side, so WP-06 sole-owner completion remains open. PR #168 strict-banks the P2 direct-response derivation/session-invalidation correction on current main; room sessions, mutations, UI, RF, and hardware acceptance remain absent.

**Required action:** implement authenticated request/response sessions with allowlisted commands, timeout, replay protection, credential handling, confirmation, and official-peer proof.

---

### F-16 — BLE, OTA/update, and local GPS are not implemented  
**Severity:** High against “all features” completion

- BLE build support is disabled.
- The partition table has one giant factory application and no OTA slots.
- The D1L has no onboard GPS.

**Required action:** implement BLE as a later isolated workstream; design a safe partition/update migration before any OTA code; support manual/companion/peer-advert location and never claim onboard GPS.

---

### F-17 — Current partition layout cannot support conventional dual-slot OTA  
**Severity:** Critical for update feature  
**Evidence:** one factory app consumes nearly the entire 8 MB flash; only default NVS/PHY and retained partitions remain.

**Impact:** “add OTA” is not a small feature. Partition-table migration can brick devices or lose retained data.

**Required action:** first produce a size/partition decision record. Either fit dual slots and require one-time USB migration, or implement a separately audited SD-staged recovery architecture. Never silently rewrite the partition table in normal app code.

---

### F-18 — Action and dependency references are not fully immutable  
**Severity:** High supply-chain risk  
**Evidence:** workflow uses moving major tags for third-party actions and an image tag rather than a digest; some host dependencies are installed without a complete lock.

**Impact:** the same source SHA can execute different build tooling later.

**Required action:** pin every action to a full commit SHA, lock Python packages/tool versions, record the IDF image digest, restrict token permissions, add CODEOWNERS for workflows, and generate provenance/SBOM.

---

### F-19 — Release automation currently defaults a helper peer to COM11  
**Severity:** Critical operational safety conflict  
**Impact:** an unattended command can open a forbidden device.

**Required action:** eliminate the default. A peer port is mandatory and must pass a forbidden-port guard. Update every script, dry-run, README example, and release gate.

---

### F-20 — Documentation and evidence can become stale faster than the code  
**Severity:** High  
**Evidence:** current docs contain detailed predecessor-SHA claims while the active head and test count moved again.

**Impact:** Codex can mistake historical proof for current closure.

**Required action:** generate a machine-readable completion ledger from current Git/CI/evidence state and render human docs from it where practical.

---

## 8. Feature-completeness matrix

| Domain | Current state at `07322ed` | Completion target |
|---|---|---|
| Boot / board / display / touch | Strong, hardware-tested on prior exact heads | Exact final-SHA cold/warm boot, touch, display, reset and brownout-safe evidence |
| Home/navigation | Partial but mature | Compact stable shell, unread/status cards, no dead actions, modular controllers |
| Public messages | Working/hardware-proven on earlier line | Full pinned-upstream vectors, multi-channel integration, final RF proof |
| Direct messages | Partial | Bidirectional ACK/PATH, direct/flood, retries, delivery truth, dedupe/replay, reboot semantics |
| Contacts | Partial | Canonical signed identity lifecycle, QR/export/import, favourite/mute, safe delete, paging |
| Heard nodes | Partial | Role/type/signal/age truth, canonical promotion, signed location markers |
| Channels | Essentially Public-only | Bounded multi-channel model, import/export, unread/history/search, protocol parity |
| Routes | Partial | Direct/flood selection, PATH learning/expiry, fallback, reciprocal path, diagnostics |
| Trace | Test-token behavior | Real protocol trace/path-discovery and correlated hop UI |
| Packets | Strong diagnostics | Deeper semantic decode, long SD journal proof, stable terminal/tool integration |
| Radio profile | Working | Final exact-SHA apply/readback and safe validation |
| Identity/adverts | Partial | Full signed advert vectors, location/role semantics, persistence/recovery |
| Repeater/room browser | Read-only | Authenticated login/session/status/admin actions with confirmation |
| SD bridge/core files | Strong but current repair unqualified | Exact pair proof, removal/reinsert, multi-card/non-FAT32/power-loss matrix |
| Retained history | Advanced but unfinished | Shared invariants, all producers coalesced, power-loss, schema/reset/time completion |
| Map | Advanced partial | Exact hardware fetch/render/pan/zoom/cache/cancel, peer pins, policy/provider lifecycle |
| Wi-Fi | Partial | Reconnect/backoff/error UX, current SDK final proof, privacy and credential handling |
| BLE companion | Not started | GATT transport using existing framed compatibility layer, pairing, coexistence and soak |
| Notifications | Minimal badges | Unified unread model, visual/backlight policy, quiet hours; no false audio claim |
| Emoji/UTF-8 | Incomplete | Curated glyph set, input/render/truncation tests, 138-byte safety |
| QR sharing | Partial display/export | Hardware display proof, import path through BLE/USB/manual payload; no camera claim |
| Terminal/log view | Serial only/diagnostic UI fragments | Bounded redacted log ring, UI terminal, USB mirror, safe command palette |
| Update/recovery | Not started | Signed package, partition decision, rollback/power-loss/recovery, SD update path |
| Local GPS | Unsupported hardware | Manual own location, companion-provided location, signed remote advert pins |
| Observer/MQTT | Setting exists, runtime not release-ready | Hide/disable or implement explicitly with opt-in/privacy/TLS |
| Diagnostics/crash/health | Strong | Structured heartbeat/task/heap/store metrics and final long-soak evidence |
| Packaging/release | Strong foundation | Immutable dependencies, SBOM/provenance, final exact-tag evidence bundle |

**Reproducible progress report, not a gate waiver:** classifying these 27 domains at the current live checkpoint as 7 implementation-complete, 13 advanced, 5 partial, and 2 not started under a 100/90/60/0 rubric yields `(7*100 + 13*90 + 5*60 + 2*0) / 27 = 80.37%`, reported as **80% capability implementation**. Applicable exact-main automated acceptance is 100%, while actual final release-gate closure is 0 of 11. The reporting weight `0.8*80.37 + 0.1*100 + 0.1*0` yields **74% weighted full-release progress**. `docs/COMPLETION_LEDGER.yaml` is the validator-backed source for this estimate; release readiness remains fail-closed.

---

## 9. Definition of “finished”

To work quickly without lying about scope, use two shipping gates but one continuous completion program.

### 9.1 Release Gate A — Stable Core (`v1.0.0` candidate)

This is the first safe public binary. It must include:

- current platform/SDK/SD/Map stack landed on `main`;
- truthful Public and DM behavior;
- canonical contacts/heard nodes;
- multi-channel support;
- real route/PATH/trace;
- authenticated repeater/room administration if it remains in the declared 1.0 contract;
- durable retained state and truthful time;
- modular/stable primary UI;
- Wi-Fi and Map final proof;
- no visible no-op BLE/OTA/GPS/observer controls;
- complete release evidence.

### 9.2 Release Gate B — Full Feature Completion (`v1.1.0` candidate)

The project is not considered “all features complete” until it additionally includes:

- BLE companion transport;
- notification and curated emoji polish;
- production terminal/log view mirrored to USB;
- signed SD/OTA update and recovery path;
- deeper/provider-capable Map zoom policy where permitted;
- optional companion-provided own location;
- any explicitly accepted observer/MQTT feature;
- full documentation and upgrade path from `v1.0.0`.

This split allows a safe public core sooner without allowing Codex to stop before the requested complete feature set.

---

## 10. Target runtime architecture

Implement this as a strangler pattern around existing code.

```text
Hardware callbacks
  ├─ SX1262 RX/TX callback ──> bounded radio_event_queue
  ├─ RP2040/UART replies ────> storage_service
  └─ Wi-Fi events ───────────> connectivity_service

mesh_runtime_task  [sole owner]
  ├─ pinned codec/crypto adapter
  ├─ advert/contact/channel directory
  ├─ DM session state machine
  ├─ route/PATH/trace engine
  ├─ authenticated admin sessions
  └─ immutable mesh_snapshot publication

storage_service / retained_worker  [sole storage policy owner]
  ├─ NVS settings/identity
  ├─ dedicated retained NVS mirror
  ├─ RP2040 FAT32 primary
  ├─ coalesced commit scheduler
  ├─ media-generation reconciliation
  └─ reboot/power-loss protocol

time_service
  ├─ monotonic protocol time
  ├─ validated wall clock
  └─ source/validity/timezone snapshot

app_command_bus
  ├─ UI commands
  ├─ USB console commands
  └─ test automation commands

ui_task  [sole LVGL owner]
  ├─ shell/navigation
  ├─ screen controllers
  ├─ immutable view models
  └─ no direct radio/storage implementation calls

diagnostics_service
  ├─ bounded redacted log ring
  ├─ heartbeat/task/heap metrics
  ├─ crash/reset evidence
  └─ USB + UI terminal consumers
```

### Required ownership rules

- callbacks never flush a store or call LVGL;
- UI callbacks never block on RF, SD, HTTP, or long NVS work;
- all protocol session transitions occur in `mesh_runtime_task`;
- all retained writes are requested through one coalescing scheduler;
- all RP2040 commands use one transaction owner and explicit cancellation/deadline;
- all snapshots are immutable for the consumer;
- every bounded queue reports depth, drops, high-water mark, and last error;
- reboot first stops new work, then drains/forces bounded commits, quiesces transport owners, records the result, and restarts only before the shared deadline expires.

---

## 11. Optimized execution sequence

### Phase 0 — Freeze, prove, and land the active stack

1. Freeze PR #80 to SD/reboot defect-only changes.
2. Run exact current ESP32/RP2040 pair evidence.
3. Fix only failures attributable to the current candidate.
4. Merge PR #62.
5. Retarget/rebase-by-merge PR #64 onto the new `main`; rerun all Actions.
6. Merge PR #64.
7. Retarget PR #80 onto the new `main`; rerun all Actions and verify the diff.
8. Merge PR #80.
9. Run a minimal exact-merged-main smoke/SD/reboot/Map-open gate.
10. Archive the stale feature branch after verifying no unique desired commit remains.

**No new feature branch should be based on the old PR stack after this phase.**

### Phase 1 — Establish contracts before features

1. Create the completion ledger and release gate IDs.
2. Pin Actions/dependencies and produce provenance.
3. Define application commands, immutable snapshots, error/result types, and capability flags.
4. Build the pinned upstream MeshCore oracle.
5. Move radio callback work behind the runtime event queue.

### Phase 2 — Complete protocol semantics

1. Full signed advert/Public/DM/ACK/PATH vectors.
2. DM state machine.
3. contact lifecycle and multi-channel model.
4. path learning/direct fallback/real trace.
5. duplicate/replay/lifetime controls.
6. two-radio targeted acceptance only after host conformance is complete.

### Phase 3 — Complete durability and truthful system state

1. shared retained commit/reconcile invariants;
2. settings schema/downgrade protection;
3. time service;
4. Wi-Fi reconnect and safe mode;
5. storage power-loss/remove/reinsert matrix;
6. crash/heartbeat telemetry.

### Phase 4 — Modularize and finish user workflows

1. extract shell and screen-controller contracts;
2. Messages redesign;
3. Nodes/Network/Map marker integration;
4. Packets/Tools/Terminal;
5. settings/connectivity/storage/update UX;
6. accessibility, UTF-8/emoji, QR, notifications.

### Phase 5 — Complete remote administration and full feature train

1. authenticated repeater/room sessions and admin actions;
2. BLE companion;
3. signed SD/update/recovery;
4. optional observer and companion location;
5. deeper/provider-capable Map policy if accepted.

### Phase 6 — Freeze, qualify, and release

1. code freeze;
2. final source/security/license review;
3. full exact-commit Actions;
4. final hardware, RF, SD, Wi-Fi/Map, BLE/update, UI, and soak gates;
5. release-candidate package;
6. clean upgrade/downgrade/recovery test;
7. tag immutable commit;
8. publish release notes, checksums, SBOM, provenance, known limitations, and support procedure.

---

## 12. Work packages

Each work package is deliberately narrow enough for one primary PR. A work package may contain multiple small PRs when called out, but no PR should mix unrelated runtime, UI, and hardware behavior.

### WP-00 — Create the live completion ledger

**Issues:** coordination/meta; supersedes stale checklist interpretation  
**Depends on:** none  
**Lane:** Release/coordination

**Implement**

- Add `docs/COMPLETION_LEDGER.yaml` with:
  - current `main`, active branch, PR, and pinned upstream SHAs;
  - every work package and dependency;
  - open/blocked/in-progress/host-green/hardware-green/merged/released state;
  - required evidence filenames and latest valid exact-commit receipt;
  - release-gate status;
  - accepted product decisions.
- Add a validator that fails CI for:
  - unknown states;
  - completed item without evidence;
  - evidence SHA not matching the declared commit;
  - dependency marked incomplete;
  - forbidden port defaults;
  - docs claiming a feature is working while the capability registry says unavailable.
- Generate a concise Markdown status page from the ledger.

**Acceptance**

- one machine-readable source of truth;
- stale evidence cannot satisfy a new SHA;
- Codex can select the highest-priority unblocked item deterministically.

**Artifact:** `completion_ledger_validation_<sha>.json`

---

### WP-01 — Close PR #80 exact-pair storage/reboot gate

**Issues:** #69, #78, PR #80  
**Depends on:** none  
**Lane:** Platform/storage  
**Scope lock:** no feature work

**Required exact inputs**

- ESP32 artifact from Actions head `07322ed...` or its defect-only successor;
- RP2040 UF2 from the same Actions run;
- verified nested checksums;
- D1L port and RP2040 port explicitly supplied;
- FAT32 card prepared on a computer;
- no Public RF and no formatting.

**Test sequence**

1. cold power cycle with card inserted;
2. verify `READY_SD`, file ops, atomic rename, marker/anchor/sentinel truth;
3. run canonical file canary;
4. run Public/DM/route/packet retained-history acceptance;
5. perform at least five immediate controlled retained-canary reboot cycles:
   - each reboot must be `SW`;
   - no WDT/PANIC/brownout;
   - no pending dirty data after recovery;
6. keep card inserted and poll status long enough to exercise GPIO7 sampling:
   - zero false `no_card`;
   - zero unintended backend generation;
7. physically remove card:
   - bounded transition to NVS fallback;
   - no repeated reset storm;
   - no delete/rename against replacement media;
8. reinsert card:
   - deterministic remount;
   - generation change;
   - read/merge before overwrite;
   - all stores reconcile;
9. repeat remove/reinsert at least ten times with writes before, during, and after edges;
10. run a two-hour storage-active soak with route/message/packet dirty events and controlled reboots;
11. export health/crash/storage/retained stats.

**Failure policy**

- preserve the exact failing artifact;
- do not clear crash logs before evidence capture;
- patch the smallest responsible layer;
- rerun host/Actions before touching hardware;
- do not change unrelated UI/protocol behavior.

**Acceptance**

- zero PANIC/WDT;
- zero false absence with inserted media;
- true removal detected;
- all retained stores and map/export paths recover;
- no user data loss;
- release audit recognizes the exact receipts.

**Artifacts**

- `sd_inserted_stability_<sha>_<d1l>_<rp2040>.json`
- `sd_remove_reinsert_<sha>_<d1l>_<rp2040>.json`
- `retained_reboot_matrix_<sha>_<d1l>.json`
- `storage_active_soak_<sha>_<d1l>.json`

---

### WP-02 — Land and normalize the PR stack

**Issues:** PR #62, #64, #80, #63, #78  
**Depends on:** WP-01  
**Lane:** Integration

**Implement**

- bank exact-head evidence before changing branch topology;
- merge #62 to `main`;
- retarget #64 to `main`, inspect the new diff, run full Actions, merge;
- retarget #80 to `main`, inspect the new diff, run full Actions, merge;
- create a temporary release-integration tag or signed note for each landed layer;
- verify current `main` contains every required commit;
- close superseded draft PRs and update issue links;
- compare stale feature branch against `main`, cherry-pick only proven unique desired changes, then archive it;
- run a minimal exact-merged-main board/UI/SD/reboot/Map-open qualification;
- update README and ledger from “candidate stack” to “main baseline.”

**Acceptance**

- no feature development remains based on an unmerged parent PR;
- `main` has green required checks;
- exact merged-main artifact is flashed and minimally proven;
- every active issue references the new baseline.

**Artifact:** `integration_baseline_<main-sha>.json`

---

### WP-03 — Make CI and release inputs immutable

**Issues:** #17, #71, new supply-chain issue  
**Depends on:** WP-02  
**Lane:** Release/security

**Implement**

- pin all `uses:` actions to full commit SHAs;
- pin the ESP-IDF container by version and digest;
- lock Python test/tool dependencies with hashes;
- record Arduino core, board package, compiler, and submodule SHAs;
- set top-level `permissions: contents: read` and elevate only where required;
- add CODEOWNERS for workflows, partition table, release scripts, and update/security code;
- produce CycloneDX or SPDX SBOM for source/package;
- produce build provenance/attestation;
- make release package include:
  - source SHA;
  - submodule SHAs;
  - toolchain lock;
  - partition table hash;
  - firmware and RP2040 checksums;
  - notices/licenses;
  - capability manifest;
  - evidence index;
- add reproducibility comparison across two Actions runs where practical.

**Acceptance**

- rerunning a source SHA uses immutable third-party inputs;
- package consumers can verify source/tool/artifact lineage;
- no workflow has broad write permissions by default.

**Artifacts**

- `build_inputs_<sha>.json`
- `sbom_<sha>.spdx.json`
- `provenance_<sha>.json`

---

### WP-04 — Define the MeshCore integration boundary and upstream oracle

**Issues:** #65  
**Depends on:** WP-02  
**Lane:** Protocol

**Implement**

- document which behavior comes directly from pinned upstream and which is locally implemented;
- create a host C++ oracle target around pinned MeshCore:
  - mock radio;
  - deterministic RNG;
  - deterministic RTC/millisecond clocks;
  - packet manager/tables;
  - contact/channel fixtures;
- expose a narrow C-compatible vector interface for:
  - identity and signed adverts;
  - Public/group packets;
  - DM encrypt/decrypt;
  - expected ACK;
  - ACK, multi-ACK, ACK+PATH;
  - direct/flood headers;
  - path return and route codes;
  - trace/path-discovery;
  - login/request/response/admin frames;
- keep production integration selectable:
  - reuse upstream implementation directly where feasible;
  - otherwise require a golden vector for every local semantic path.
- add a policy check that a MeshCore submodule update cannot merge without new vector review and corpus version.

**Acceptance**

- a deterministic pinned-upstream executable oracle exists in CI;
- local protocol code cannot add a packet type without a vector;
- the integration boundary is understandable to a new maintainer.

**Artifact:** `meshcore_oracle_manifest_<sha>.json`

**Live partial evidence:** PR #101 merged bounded explicit-loop TRACE request/correlation/retention software evidence as `trace_discovery_4a85e827c251c6d1d22d276c7a40d71571b23563.json`. PR #102 merged `admin_host_fixture_6c096afbb5c791bf661ff146c6fbcb1f4852d2da.json`, covering a pinned host-only authenticated transcript with six positive and sixteen fail-closed negative checks. PR #104 merged fail-closed production identity startup as `production_identity_binding_16db6055f47541756f79edd06530d0cd1a6c878b.json`. Corrected PR #105 merged exact-full-key verified-advert contact promotion as `verified_advert_contact_binding_e7ddb265a0a84e7ecc3860bebf959d9551fdb00a.json`; exact-main receipt SHA-256 `d9b2dc5efbcddd787260cc6a6e11cd90e3433fcbd3f7ec971f7a498b7b79eab7` banks the software/artifact gate. These receipts explicitly do not satisfy full retained contact lifecycle/recovery, concrete packet-pool admin dispatch, generic contact TRACE, multi-hop/official-peer RF, physical, WP-18, or WP-04 closure.

---

### WP-05 — Complete semantic conformance and fuzzing

**Issues:** #65  
**Depends on:** WP-04  
**Lane:** Protocol/QA

**Vector matrix**

- valid/invalid signed adverts, names, roles, optional lat/lon;
- Public and every configured channel;
- DM attempts 0–255 as supported, including extended attempt byte;
- shared-secret derivation;
- MAC/authentication failure;
- direct/flood transport codes;
- ACK, multi-ACK, flood ACK+PATH;
- path hash sizes/counts and maximums;
- requests, responses, login, keepalive, status;
- trace/path discovery;
- replayed advert timestamp;
- duplicate packet/hash;
- truncated/oversize/empty payload;
- malformed UTF-8 and embedded NUL;
- self-message and unknown peer/channel;
- lifetime/expiry boundaries.

**Fuzzing**

- raw wire decoder;
- semantic packet parser;
- decrypt/auth path;
- advert parser;
- DM/session transition input;
- retained envelope decoder;
- route/path parser;
- USB command parser for protocol commands.

Use sanitizer builds in normal CI with a bounded deterministic corpus and a longer scheduled fuzz job.

**Acceptance**

- SIGUI-generated packets are accepted by pinned upstream and vice versa for the full declared surface;
- invalid cases fail closed;
- zero sanitizer finding;
- `closure_ready=true` is allowed only after full vector counts are present.

**Artifact:** `meshcore_conformance_<sha>.json`

**Live partial evidence:** PR #176 head `40e1d8c8bd1ab05dc754338c1d2815e6aefdb682`, synthetic merge `208894df13b994c8eae2906eb84262411f204a63`, and exact main `3f0d8d55b7a67c025fa8b8405db59ed353f68dc8` share tree `812f29908c8b7051ae50eb3fb05eda6c5bf4748e`. Canonical PR Actions `29535402995` and exact-main Actions `29536138152` pass; strict PR/main receipt SHA-256 values are `8001ae7af56f8e3e3124df2f6051afd38de0baee9db9e8ff68128d245d58f6b4` and `8bcde9329fa8832944dd6f4ca325fc79639de0d4c096f5b9e9b3134268d322e4`. Portable aggregate `docs/completion/evidence/wp05/text_admission_3f0d8d55b7a67c025fa8b8405db59ed353f68dc8.json` has SHA-256 `3365d886580a4f4fa72f7cc0a91ad5ad9f7d306510b88a5aed2138816f057c29`. This preserves PR #170's complete declared matrix inventory and adds executable production strict text admission, advancing coverage to 7 implemented / 8 partial / 1 missing semantic requirements, 2 implemented / 1 partial / 5 missing fuzz targets, 6 suites / 34 scenarios, and 46 pinned compiler-bound dependencies. Official-peer RF, physical acceptance, WP-05, and release acceptance remain open.

---

### WP-06 — Establish one Mesh runtime owner

**Issues:** #6, #16, portions of #69  
**Depends on:** WP-02, WP-04  
**Lane:** Runtime

**Implement**

- add `mesh_runtime_task`;
- reduce SX1262 callbacks to:
  - copy bounded metadata/raw bytes;
  - timestamp with monotonic source;
  - enqueue event;
  - return;
- define bounded commands:
  - start/stop RX;
  - send Public/channel;
  - send DM;
  - cancel/retry DM;
  - advert;
  - trace/path discovery;
  - admin login/request/logout;
  - apply radio profile;
- define bounded events:
  - RX packet;
  - TxStarted/TxDone/TxTimeout;
  - radio error;
  - timer;
  - storage commit result;
  - shutdown;
- publish immutable snapshots for UI/console;
- add queue depth/high-water/drop/error telemetry;
- make queue saturation an explicit caller error;
- ban direct store flushes and LVGL calls from callbacks;
- add task stack watermark and watchdog heartbeat.

**Migration method**

1. wrap existing functions behind commands;
2. move one packet type at a time;
3. retain old entry points as thin adapters;
4. delete old direct paths only after contract tests prove parity.

**Acceptance**

- all protocol/session state transitions happen on one task;
- callback stack and execution time are bounded;
- ThreadSanitizer-style host model tests or deterministic concurrency tests cover command/event interleavings;
- no UI or console code accesses mutable service internals.

**Artifact:** `mesh_runtime_ownership_<sha>.json`

**Live partial evidence:** PR #106 head `7a86e15b8584b76fd9f0ae86f6bbabdcde6930e6` merged as `db58229aaec8cc5649b94aeb3cefe6af598203a8`; exact-main callback aggregate `radio_callback_queue_db58229aaec8cc5649b94aeb3cefe6af598203a8.json` has SHA-256 `94fb099b83cd69c8101561c13dcdb76af9cd3e3c944199e4dd3afc2753ff2d85`. PR #109 then merged runtime-owned identity/readiness/timestamp/advert/RF admission and zero-wait deferred packet-log admission on predecessor main `bf9ba087e1d863a2596561cb6b3de015c606c598`; aggregate `advert_dispatch_owner_bf9ba087e1d863a2596561cb6b3de015c606c598.json` has SHA-256 `3705fc71ef9d3cc210a91c08a78f90c1a60959491eef1aad61171b71a0ccfea7`. PR #171 head `22944c9fef7fb44f27ff292024e6bbdd224b9cbd`, synthetic merge `79123bcaa98e9d9fd45396bcae53824b07b4a508`, and exact main `2d360b6697d4028a68f23f66bbbe3d51b0965a6b` share tree `f83d29f9d885bb92f540c897a98ac1f600232cfb`; PR/exact-main Actions `29527034103` / `29527676305` pass, and exact-main receipt SHA-256 is `9927bd68071295d6b26446e26277c64b7a40afd7395c024aeef6f30135f2e7f9`. It adds bounded runtime-owned Admin login/status/logout and deadline/saturation/recursion/stale-generation/zeroization guards. PR #173 head `1e93be8ed9a33a48dfbe89af4c64b08b5ba04e91`, synthetic merge `6ce4e010241cfba17ba293a74985361c39a9cfe4`, and exact main `afd3c03e7f5f6358f1f08f7c1cd19b9aef22f929` share tree `a7b14f729c8706ca1f6fe19844dfdf61d844eab3`. PR/exact-main Actions `29530513204` / `29531468019` pass; PR/main receipt SHA-256 values are `154d99ba4104273b8ea64218c45c69d5b46ca958e9e1e9be7f8518c68a0931a0` and `f13f633d7d526b88cef5871cae3627ed27ab626437be7a17f7d4a6c4880ab2f7`. Portable aggregate `queue_fairness_afd3c03e7f5f6358f1f08f7c1cd19b9aef22f929.json` has SHA-256 `eb773cd5609f83dc446fb7944ef69cb28d8f36417575591b1e918473144c1e4f`. This adds bounded priority-lane fairness, terminal precedence, accepted-command preservation, TX admission/RX recovery guards, and saturating telemetry. Failed Clang LP64 run `29530086294` is superseded negative evidence only. Complete remaining command/event ownership, watchdog/reboot/power-loss recovery, remote mutation/downstream integration, UI ownership, hardware, RF, and WP-06 closure remain open.

---

### WP-07 — Implement truthful DM ACK/retry/delivery

**Issues:** #66, #7  
**Depends on:** WP-05, WP-06  
**Lane:** Protocol

**Persistent state model**

`QUEUED → WAITING_RADIO → TX_ACTIVE → TX_DONE → AWAITING_ACK → ACKNOWLEDGED`

Failure/recovery branches:

- `RETRY_WAIT`
- `RETRY_TX`
- `FAILED_RADIO`
- `FAILED_TIMEOUT`
- `FAILED_QUEUE`
- `INTERRUPTED_BY_REBOOT`
- `CANCELLED` where supported

Store:

- stable local message/session ID;
- recipient full public key;
- payload and encoded-byte length;
- timestamp;
- attempt;
- expected ACK;
- route mode/path generation;
- deadline;
- transition reason;
- last radio/error code;
- retry count;
- durable revision.

**Inbound behavior**

- authenticate before visible storage;
- dedupe by authenticated sender/timestamp/attempt/payload identity;
- generate ACK exactly as pinned upstream;
- flood inbound: send ACK+PATH/return path as required;
- direct inbound: direct ACK;
- duplicate valid inbound: do not create another row; optionally bounded re-ACK per upstream compatibility;
- malformed/unauthenticated/replayed: no visible message and no incorrect ACK.

**Outbound behavior**

- prefer valid direct route;
- fall back to flood on unknown/expired/failed path according to policy;
- transition on actual radio callbacks;
- correlate only the expected authenticated ACK;
- bound attempts and total lifetime;
- preserve truthful final state across reboot.

**UI**

- queued, sending, sent-over-RF, delivered, retrying, failed;
- explicit retry action;
- error details in technical disclosure, not primary bubble;
- no green “sent” merely because queue accepted bytes.

**Acceptance**

- host state-machine matrix;
- loss/duplicate/reorder/failure injection;
- exact two-radio targeted DM in both directions;
- no Public RF;
- no duplicate visible messages or ACK storm;
- deterministic reboot result.

**Artifact:** `dm_session_acceptance_<sha>_<d1l>_<peer>.json`

---

### WP-08 — Canonical contacts, heard nodes, and QR lifecycle

**Issues:** #67, #20, #75  
**Depends on:** WP-05, WP-06  
**Lane:** Protocol/application

**Implement**

- separate:
  - transient heard observation;
  - signed advertised node;
  - canonical contact;
  - local alias/preferences;
- canonical key is full public key, never short fingerprint alone;
- record role/type, signed advert timestamp, last heard, path, signal, location, verification source;
- promotion paths:
  - signed advert;
  - verified QR/payload import;
  - explicit import over BLE/USB;
- enforce role capability:
  - chat contact can DM;
  - repeater/room can login/admin where supported;
  - unknown type has no dead Message/Admin action;
- bounded eviction:
  - never evict favourites without confirmation;
  - preserve aliases/preferences across advert refresh;
- QR:
  - display MeshCore-compatible contact payload;
  - import through text/BLE/USB because the D1L has no camera;
  - validate length, scheme, key, role, and signature/advert packet where applicable;
- add paging/search and duplicate merge rules.

**Acceptance**

- heard-only row cannot accidentally send DM;
- signed advert promotes deterministically;
- import/export round-trip against official client;
- favourite/mute/alias survive reboot and refresh;
- contact deletion is scoped and confirmed.

**Artifact:** `contact_lifecycle_<sha>.json`

**Live partial evidence:** PR #112 head `79e1e7d8c06917ce011f32ee8e345fef40731e0e` merged as `10d85ee3a0941aff23f455047358805a861b571e`, sharing tree `4368725b7abbde0d40b1886ba24a9e376cd30674`. Exact-main Actions `29359402515` and strict receipt `53e07c470b01a46ffcc2414c4e5b9867da9932b11203259a3d0e4e48cd3f78dc` bank strict bounded MeshCore URI import/export, exact-key provenance, heard-only/collision/capacity fail-closed behavior, favourite/alias preservation, canonical DM authorization, and injection-safe USB contact JSON. Portable aggregate `contact_lifecycle_10d85ee3a0941aff23f455047358805a861b571e.json` has SHA-256 `30604a5d185c3aaec607e13db9e98dc5b2303f1e7b9ccc7373a6495bd5b8b7c4`. BLE and remaining on-device QR/share surfaces, official-client bidirectional interoperability, retained reboot/advert-refresh proof, paging/search completion, physical/RF evidence, downstream channel integration, and WP-08 closure remain open.

---

### WP-09 — Multi-channel model and messaging

**Issues:** #67, #20, #74  
**Depends on:** WP-05, WP-06, WP-08  
**Lane:** Protocol/application

**Implement**

- bounded channel collection with schema:
  - stable channel ID;
  - display name;
  - secret/key material;
  - enabled/default;
  - source/import metadata;
  - unread cursor/history key;
- keep built-in Public as an explicit channel, not a special hard-coded global path;
- support add/edit/import/export/remove with confirmation;
- never print secrets to logs, screenshots, crash dumps, or normal exports;
- route all channel TX/RX through the same semantic conformance layer;
- per-channel history/search/unread;
- compose byte counter and 138-byte limit;
- optional signed sender display where protocol supports it;
- channel data payloads remain hidden until separately supported.

**Acceptance**

- at least Public plus multiple user channels pass upstream bidirectional vectors;
- reboot/upgrade preserves channels;
- wrong key does not expose garbage as a message;
- no secret leakage in diagnostics.

**Artifact:** `channel_acceptance_<sha>.json`

**Live partial evidence:** PR #114 banks the bounded durable Public-plus-seven channel model, PR #116 banks atomic redacted metadata/selection through the app/USB boundary, PR #152 banks selected-channel runtime/storage, and PRs #153-#155 add truthful channel conversations and the redacted confirmation-gated management app boundary. PR #157 then banks secure channel create/import/manage/rename/enable/default/remove/one-shot QR sheets and the fail-closed hardware-entropy-backed DRBG at exact main `404a2fa9e3b28b86d3118920753c85fdd5e0e95e`. Retained PR Actions `29451639572` and exact-main Actions `29452355824` pass; exact-main receipt SHA-256 is `35319a7b0a5e7e527846121e8bc30f6456532650b816fd7026e4b01c17be4144`, gate-manifest SHA-256 is `57ff3427cabf3ebbfd169db6cb99202763748ca39e70e536a6e42e69f687e045`, and portable aggregate SHA-256 is `9b53434d25d3c7d7896c8263c50cf96b56e17000cc51dbf8bafcf2a5e39d9342`. Official-client bidirectional RF, exact-candidate reboot/reset/recovery and physical UI evidence, the required acceptance artifact, dependency closure, WP-09 closure, and release closure remain open.

---

### WP-10 — Real PATH, direct fallback, and trace

**Issues:** #68, #19, #7  
**Depends on:** WP-05, WP-07, WP-08  
**Lane:** Protocol

**Implement**

- canonical path record:
  - peer key;
  - path bytes/hash mode;
  - source (advert, ACK+PATH, path response, observed);
  - learned time;
  - last success/failure;
  - expiry;
  - generation;
- validate path length/hash encoding against upstream;
- direct route only while valid;
- on direct failure:
  - increment failure;
  - expire/reset path at threshold;
  - retry flood within DM policy;
- process reciprocal return paths;
- implement actual trace/path-discovery request and correlated response;
- trace UI shows:
  - in progress;
  - hop sequence/hash width;
  - response age;
  - timeout/failure;
  - route mode;
- keep privacy limits: do not pretend path hashes reveal node identity unless a verified mapping exists.

**Acceptance**

- upstream vector parity;
- direct success, expired path, failed direct→flood fallback;
- ACK+PATH learns a usable route;
- trace is not a text DM;
- targeted RF proof against compatible peer.

**Artifact:** `route_trace_acceptance_<sha>_<d1l>_<peer>.json`

---

### WP-11 — Consolidate retained durability, schema, reset, and power loss

**Issues:** #69, #4, #78  
**Depends on:** WP-01, WP-02, WP-06  
**Lane:** Storage

**Do not begin structural refactor until WP-01 evidence is green.**

**Formal invariants**

- SD primary and NVS fallback have explicit authority and lineage;
- media generation is captured before a write and rechecked before replace;
- a replacement card is never cleaned up using an old-media transaction;
- foreign removable lineage cannot overwrite authoritative device-local cleared state;
- every envelope has magic, schema, length, revision/epoch, payload checksum, and optional source lineage;
- unknown newer schema is preserved and quarantined, not reset;
- commit scheduling is coalesced and observable;
- forced reboot flush has one shared deadline;
- failure to quiesce cancels reboot and releases all locks;
- no hot RX path performs full synchronous store serialization.

**Implement**

- common retained-store descriptor and scheduler;
- common dirty/reconcile/commit result state;
- store-specific serialization stays isolated;
- global producer quiescence registry;
- power-loss fault injection at every write/rename/marker/sentinel stage;
- NVS capacity/write-amplification telemetry;
- scoped clear/reset:
  - messages;
  - routes;
  - packets;
  - contacts;
  - settings;
  - Wi-Fi;
  - full factory reset;
- user-visible recovery state and export.

**Acceptance**

- all four stores pass the same transition matrix;
- no write storm under packet bursts;
- power interruption always yields old valid or new valid state, never accepted corruption;
- downgrade/newer schema is non-destructive;
- removal/reinsert and reboot evidence remains green.

**Artifacts**

- `retained_invariants_<sha>.json`
- `retained_powerloss_<sha>.json`
- `nvs_write_amplification_<sha>.json`

**Live partial evidence:** PR #118 banks the shared retained scheduler/deadline boundary with portable aggregate SHA-256 `8d2991f4ce60d4482610ba606540bf17f1942d32a8ce93cbdfaba4317e203847`. PR #154 banks boot-local retained-NVS telemetry. PR #158 head `8e0dd27c5351a1d16cadb3c5a4307489ac3957cd` merges as `3de6e8d634bd76fc52e5e4bf98f93fc48adf6648`; exact-main Actions `29456320080`, receipt SHA-256 `dcb2c1dcfd7b64091609c1b686208f247a38d45905b62196c964331b45787086`, and aggregate SHA-256 `eb82dd9f8763812597f6d9dda0c3e783594c5666b3b23abb8c5375670c1fbb2e` bank fail-closed versioned settings. PR #174 head `8de50dcd26a6468aee05986acc9b59e09a274a58`, synthetic merge `90f88ff692dbb426077ca7188d2fe87c9769332a`, and exact main `9686c3d7fa52bb1b3c236b930a8c7383527125ac` share tree `49dd5a7d53fa97b8badc9cfe79bf2a254680798a`. PR/exact-main Actions `29534103735` / `29534773905` pass; exact-main receipt SHA-256 is `2977fe7594a9d1d149f23213a2638b39b1a3b7fff6b11724ee747d0aa94799e9`, gate-manifest SHA-256 is `cbf6ed19a0b0fcfb1a90c7c419d5f0055e63a7f2657c7813924dcd044a03bbd0`, and portable aggregate `factory_reset_9686c3d7fa52bb1b3c236b930a8c7383527125ac.json` has SHA-256 `4375a6dee581ffc4507c3be11c77edb192455c16a08a75fcf30c40e7f3e5c39d`. This adds a producer-quiesced, durable-journal, early-boot-resumable exact-key reset that clears only owned paths, preserves safety guards, fences four stale removable-SD lineages, and never formats SD. Physical interruption/repeated-power-loss, absent/insert/swap/remove/reinsert/reboot card-lineage acceptance, the bridge one-delete replacement window, named-workload NVS write amplification/endurance, exact-candidate recovery/durability, and WP-11 closure remain open.

---

### WP-12 — Truthful time service

**Issues:** #69, P0.19 audit item  
**Depends on:** WP-02, WP-11  
**Lane:** Platform

**Implement**

- separate clocks:
  - monotonic boot time;
  - unique protocol timestamp allocator;
  - validated wall clock;
- wall-clock sources:
  - SNTP over Wi-Fi;
  - optional companion-set time;
  - safe bootstrap lower bound from retained authenticated data;
- validity states:
  - unset;
  - monotonic-only;
  - approximate;
  - network-validated;
  - companion-validated;
- persist last validated time and monotonic guard without excessive writes;
- timezone setting and display conversion;
- never allow wall clock to move protocol uniqueness backward;
- admin/session expiry uses monotonic deadlines where possible.

**Acceptance**

- cold boot without network is honest;
- SNTP transition updates UI without duplicate protocol timestamps;
- backward/forward clock jumps are handled;
- reboot preserves monotonic uniqueness;
- Map TLS waits for valid certificate time but remains cancellable.

**Artifact:** `time_service_acceptance_<sha>.json`

**Live partial evidence:** PRs #120, #124, and #160 bank centralized time ownership, reserve-before-use protocol high water, exact-source anchoring, bounded SNTP quarantine, retained-wall recovery, protocol preflight, and truthful diagnostics. PR #162 head `b7944f325882225fbe622798c7b6765d0b6e6a8a`, synthetic merge `f8f89f73275e7986e1314467ce7c45d25e4169f2`, and exact main `aeb538088bada27fa8f84882b05a641fb944bc90` share tree `1bee63e301e0f6d2e03ce675b166bdeb41fa0c40`. Retained PR Actions `29462663743` and exact-main Actions `29463374435` pass 1,178 host, 32 checksum, and 74 focused tests; PR/main receipt SHA-256 values are `fa491b7435fd2eb71dbe4d968aa4a15e06845d1abd47fdf7b55634f9f3fc4718` and `7da9d5fa09a3e0a7b7e73db23aa483e0c1214bfa06b0e7a592e1dd5814ab9af9`, exact-main gate-manifest SHA-256 is `ab94aaf8ad1f03a32d67f923d2c6a23ab5789fee1b2d6939b300298b220d8e66`, and portable aggregate `timezone_display_aeb538088bada27fa8f84882b05a641fb944bc90.json` has SHA-256 `c774398c115abd6c390c2fdfd2dec45e1bf259650012005013e04d127c776f06`. This additionally banks a persisted fixed UTC offset from UTC-12:00 through UTC+14:00, including minute offsets; schema-v8 migration; overflow-safe display-only conversion; truthful USB/UI state; and UTC protocol/certificate clocks. IANA zones and automatic DST are not supported. Legacy `mesh_ts` migration, authenticated companion transport acceptance, exact-candidate physical SNTP/jump/TLS/reboot/power-interruption proof, the required artifact, WP-12, and release closure remain open.

---

### WP-13 — Wi-Fi reconnect, credentials, and safe mode

**Issues:** #13, #63  
**Depends on:** WP-02, WP-06, WP-12  
**Lane:** Connectivity

**Implement**

- explicit states:
  - off;
  - profile required;
  - starting;
  - scanning;
  - connecting;
  - connected;
  - auth failed;
  - AP unavailable;
  - retry backoff;
  - safe-mode disabled;
- bounded exponential backoff with jitter and maximum attempt window;
- user cancel/disable;
- do not permanently disable Wi-Fi for one transient disconnect;
- associate crash-loop safe mode with repeated boot failures and the last enabled subsystem;
- redact credentials everywhere;
- document physical-flash threat model and optionally evaluate NVS encryption as a separate security decision;
- expose RSSI/channel/IP without blocking;
- memory and task-stack telemetry under repeated reconnects;
- Wi-Fi/BLE coexistence policy becomes an explicit capability, not an implicit toggle side effect.

**Acceptance**

- AP reboot, wrong password, weak signal, disconnect/reconnect, saved-profile reboot, and safe-mode recovery;
- no boot loop;
- no heap leak after 100 connect/disconnect cycles;
- Map resumes only when visible and allowed;
- current ESP-IDF 5.5.4 exact-hardware proof.

**Artifact:** `wifi_resilience_<sha>_<d1l>.json`

---

### WP-14 — UI ownership and modular extraction

**Issues:** #6, #16, #76  
**Depends on:** WP-06  
**Lane:** UI

**Implement without redesigning everything at once**

1. define `ui_command` and immutable view-model interfaces;
2. move all cross-task UI requests through a UI queue;
3. extract common shell/dock/header/status components;
4. extract screen lifetime helper:
   - create;
   - activate;
   - refresh;
   - deactivate;
   - destroy;
5. extract screens in this order:
   - Home;
   - More/Settings shell;
   - Messages root/thread/compose;
   - Nodes/Network/contact;
   - Packets;
   - Storage;
   - Wi-Fi;
   - Map;
   - Tools/Terminal/Admin/Update;
6. add generation tokens for async responses so a destroyed screen cannot be updated;
7. eliminate global widget pointers after each screen migration;
8. keep deterministic simulator snapshots for each step.

**Acceptance**

- LVGL calls occur on UI task only;
- open/close loops leave no dangling object/callback/timer;
- 1,000 automated navigation transitions without panic, WDT, object error, or unbounded memory loss;
- touch targets ≥44×44 where appropriate;
- scroll/focus/keyboard contracts remain green;
- each extracted screen has focused tests and no direct radio/storage dependency.

**Artifact:** `ui_runtime_safety_<sha>_<d1l>.json`

**Live partial evidence:** PRs #108/#110/#115/#125/#127/#129-#133/#135/#136 bank the planned hierarchy/controller extraction in code, and corrected PR #159 banks the compact `Home`/`Messages`/`Nodes`/`Map`/`Tools` dock plus shared Map/chrome geometry. PR #164 head `51ffc4b3011b43bc714c8d85a47b4417ef45ba66`, retained synthetic merge `84e12bce3164f992625a602a52ee0b8549458bbc`, and exact main `8a7f65436df9e159e1d50434686c683bfb2d4e28` share tree `4957b197ba6cd26effced22163dc8c45c595913e`. Retained PR Actions `29508315885` and exact-main Actions `29509015073` pass; PR/main receipt SHA-256 values are `6127ddabeba26ee76794f30e5c2809096bb0aa9e86239f266fc29fbcceee5e97` and `a091be62f78c9c4d148a1abfcabe1285d0cd8a0e45febdca02ff3a4611e4b2a3`, exact-main gate-manifest SHA-256 is `a54468d186217582990f6402e6b91ccfe3515192cfb305c0a8b4439ecfc875c5`, and portable aggregate `home_status_actions_8a7f65436df9e159e1d50434686c683bfb2d4e28.json` has SHA-256 `770da4bc95b85df9ef7c3e67273e6bab597d23f2140dc350a0b3cdd90c6b8603`. This banks five compact, directly actionable Mesh/Wi-Fi/BLE/SD/Attention states, fail-closed status projection, and active-tab modal return only. UI-task-only LVGL proof, repeated lifecycle proof, 1,000 transitions, exact-candidate touch/scroll/focus/keyboard and pixel/accessibility review, the required runtime-safety artifact, and WP-14 closure remain open.

---

### WP-15 — Finish Messages UX

**Issues:** #74, #66, #67  
**Depends on:** WP-07, WP-09, WP-14  
**Lane:** UI

**Implement**

- Messages root has channel and DM sections with unread counts;
- Public/default channel opens first but does not hide other channels;
- recent list shows sender short name, time validity, preview, unread;
- thread bubbles show truthful delivery state;
- path/technical metadata under disclosure;
- one sticky Compose/Reply action;
- compose supports:
  - byte counter;
  - UTF-8 validation;
  - curated emoji insertion;
  - send disabled on empty/oversize/no route/contact;
- tapping a valid sender/contact can open DM; invalid heard-only identity explains why;
- search across bounded retained pages;
- empty, loading, storage-degraded, no-contact, failure and retry states;
- notification badges clear only when the relevant cursor advances.

**Acceptance**

- all simulator states;
- physical touch/keyboard/scroll;
- inbound/outbound DM state transitions visible;
- no crash switching Public/DM/channel during incoming events;
- long names/text and UTF-8 render safely.

**Artifact:** `messages_ui_acceptance_<sha>_<d1l>.json`

---

### WP-16 — Finish Nodes, Network, roles, and Map pins

**Issues:** #73, #75, #68, #70  
**Depends on:** WP-08, WP-10, WP-14  
**Lane:** UI/application

**Live partial evidence:** PR #165 head `801614311366d8274116e19599ddb6a0430fd454`, retained synthetic merge `0ccaf23429ce63537a5afa95db46fd3ec001ebc9`, and exact main `0eee2ce4032911e8b133d5a13ea8dc1715385a4b` share tree `e39266e9dda7e73024bd5f5b3380c7945c55a297`; portable aggregate `node_role_summary_0eee2ce4032911e8b133d5a13ea8dc1715385a4b.json` has SHA-256 `c0358a33cfd327e7cb448d7307e9ca3044f851d522df11450d812d71340a4119`. PR #172 head `4ceb6c6aca3a8ccdc6eb233cf4229df113cda6bf`, retained synthetic merge `20bfa6e8b2386f241355aa58aefe6b1b6e9b5e4c`, and exact main `a49d6f20967af314022c8ffbf3be0cff0fa2592f` share tree `2af4224dd78d3d62c691c7fef78408453ca24b3d`; PR/exact-main Actions `29528121033` / `29529074494` pass, with PR/main receipt SHA-256 values `fa34cd438334eb8ece40e2be260f3fda941378365d7ff24bc1d0988984fd677c` / `b8420a1593f3747d89ff268fe04d40e7dbec10b3ef44132b2019b5b534618a0d`. PR #175 head `6ae3726a1c740eb72e14320b51c313c84ed6249d`, retained synthetic merge `d4f4827528ae8b3b5110db97fdcf2eef5858e225`, and exact main `2a5cb982c967cb1185cd047838b4213ca7ba61bc` share tree `67e2b2eaac67820d9b767b099ac0f829b13af00d`; PR/exact-main Actions `29532389607` / `29533095157` pass, with PR/main receipt SHA-256 values `f1e047b1ea86041a490930fb7940294f498596618026468a2ff0b75da785cc4d` / `8ce5749a162c591739b8494a9ddfbf28d1dbb5c947ac658d5278ac878d26c75c`. Portable aggregate `signed_location_map_2a5cb982c967cb1185cd047838b4213ca7ba61bc.json` has SHA-256 `62aea4366447e69126757fc4dbe7867be3eb597c258aad4d310121460ca997c2`. These slices bank exact bounded role totals, a bounded Node Detail projection with truthful capability-gated DM/passive Admin state, Map return-state preservation, verified nonfuture signed-advert pins with exact role/age and unknown-accuracy truth, explicit center provenance, trust-loss invalidation, and backward-time rollback rechecks. Remaining capability/admin actions, real trace/network tools, max-record live-update responsiveness, Map-provider qualification, exact-candidate physical Nodes/Map proof, dependencies, the required artifact, and WP-16 closure remain open.

**Implement**

- Nodes root:
  - heard/verified/contact state;
  - role icon;
  - short/long name;
  - last heard;
  - SNR/RSSI/noise floor where valid;
  - route state;
  - favourite/mute;
- detail page:
  - full fingerprint/public-key disclosure;
  - advert verification;
  - role/type;
  - path and signal history;
  - Message only when sendable;
  - Admin only for compatible verified server role;
- Map pins:
  - only signed advert locations;
  - age/verification/role;
  - no inferred precision;
  - own center from manual or companion source;
  - no local-GPS claim;
- Network tools:
  - real trace;
  - Ping Nearby/Finder only when protocol semantics and cooldown are defined;
  - clear no-response state.

**Acceptance**

- malformed/unsigned location never produces a trusted pin;
- role actions match capability;
- node list remains responsive under max records and live updates;
- physical Map pin pan/zoom/select proof.

**Artifact:** `network_map_nodes_<sha>_<d1l>.json`

---

### WP-17 — Finish Packets, diagnostics, and Terminal

**Issues:** #18, #19, #76, new terminal issue  
**Depends on:** WP-06, WP-11, WP-14  
**Lane:** Diagnostics/UI

**Implement**

- split USB console into domain command modules and a command registry;
- one safe JSON encoder/writer;
- declare each command:
  - read-only;
  - mutating;
  - RF-transmitting;
  - destructive;
  - privileged;
- require confirmation/capability for mutating/destructive commands;
- add a bounded redacted in-memory log/event ring;
- mirror the same structured events to USB and UI Terminal;
- Terminal defaults read-only; advanced command palette uses allowlisted application commands, not arbitrary parser injection;
- improve packet semantic decode only after conformance types exist;
- expose:
  - task heartbeat/stack;
  - heap/PSRAM;
  - queue high-water/drops;
  - NVS writes;
  - SD generations/failures;
  - Wi-Fi reconnect;
  - radio errors;
  - crash/reset history;
- retain 24-hour SD scrollback proof.

**Acceptance**

- no secret/private key/password/channel key in logs;
- parser fuzz green;
- terminal cannot bypass RF/format/update safeguards;
- packet paging under full journal;
- diagnostics remain responsive during soak.

**Artifact:** `diagnostics_terminal_<sha>_<d1l>.json`

---

### WP-18 — Authenticated repeater and room administration

**Issues:** #77  
**Depends on:** WP-05, WP-08, WP-10, WP-12, WP-14  
**Lane:** Protocol/security/UI

**Live partial evidence:** PR #166 merged bounded production read-only repeater login/status behavior as exact main `11247cd2acdef5454c443994ef5f5d110b78d256`; Actions `29512097858`, receipt `c4b40caabd247ecf2abe96347d29c075711b83d2cb7b3cbce0843eb091a23625`, and corrected aggregate `docs/completion/evidence/wp04/admin_dispatch_runtime_11247cd2acdef5454c443994ef5f5d110b78d256.json` (SHA-256 `e7a139bfacb78d87bbdc0e30f709622d6f49ba276136f2fb0fbbf4b675ad73a9`) bank software/artifact integrity only. Raw Admin TX is service-queued, but Admin runtime/session parsing and mutations remain caller-side, so WP-06 sole-owner completion is still open. PR #168 exact main `c88b341abdddb87913ca6b0443804547acadca16` closes the later P2 direct-response invalidation finding; Actions `29515366961`, receipt `254ca1982f343c74d8d78c3afd6e42f9cfa02a0447d1bb8e2afdf8353e34c1eb`, and aggregate `docs/completion/evidence/wp04/contact_targeted_trace_c88b341abdddb87913ca6b0443804547acadca16.json` (SHA-256 `48cedc8dddd9dc21d8cc86c7b44816c808ca123d341408c1cabd39ca22cca615`) bank that correction and bounded contact-targeted TRACE. Room administration, mutations, UI, RF/hardware acceptance, and WP-18 closure remain blocked and incomplete.

**Implement**

- compatible server-role detection;
- login request and explicit success/failure;
- session table with:
  - server identity;
  - authenticated state;
  - activity;
  - keepalive;
  - expected response/ACK;
  - monotonic timeout;
- credentials stored separately from general display settings and always redacted;
- bounded retries and lockout/backoff;
- allowlisted read/status operations first;
- mutating operations only after:
  - authenticated session;
  - capability advertisement or known protocol version;
  - local confirmation;
  - response correlation;
- destructive operations require stronger confirmation and are disabled until specifically tested;
- never expose a raw remote CLI tunnel as the first implementation;
- logout and reboot clear volatile session secrets.

**Acceptance**

- official-compatible repeater and room test peers;
- wrong password, replay, timeout, disconnect, malformed response, unsupported command;
- no unauthenticated state change;
- credentials absent from logs/exports/screenshots;
- UI accurately shows read-only vs authenticated capabilities.

**Artifact:** `admin_session_acceptance_<sha>_<d1l>_<peer>.json`

---

### WP-19 — Finalize Map provider/cache lifecycle

**Issues:** #12, #14, #73  
**Depends on:** WP-01, WP-13, WP-14, WP-16  
**Lane:** Map/connectivity

**Implement**

- preserve current-view-only, max-nine-tile and user-opened rules;
- exact physical pan/zoom/Center/leave cancellation/cache revisit proof;
- track cache metadata:
  - provider/source;
  - fetch time;
  - minimum retention;
  - optional ETag/Last-Modified if revalidation is added;
- no area download or multi-zoom offline package from `tile.openstreetmap.org`;
- persistent visible attribution;
- stable contactable User-Agent;
- provider abstraction in code/config so URL can change without invasive Map rewrite;
- default source remains policy-compliant;
- optional deeper zoom only when user-driven and provider terms permit;
- cache inspection and safe user-initiated clear;
- corrupted tile quarantine;
- memory/PSRAM regression gate.

**Acceptance**

- first view ≤9 requests;
- revisit performs zero new request while cache is valid;
- leave cancels remaining network work;
- rate-limit obeyed;
- no hidden background fetch;
- Map works from existing cache with Wi-Fi off;
- invalid/cache-corrupt paths are truthful and non-crashing.

**Artifact:** `map_live_acceptance_<sha>_<d1l>.json`

---

### WP-20 — BLE companion transport

**Issues:** new BLE issue, companion compatibility follow-up  
**Depends on:** WP-03, WP-06, WP-07, WP-09, WP-13  
**Lane:** Connectivity/protocol

**Implement**

- enable NimBLE on the supported SDK with an explicit memory budget;
- define GATT service using the existing MeshCore 3-byte companion framing compatibility layer;
- pairing/PIN policy and bonded-device management;
- connection state, MTU, fragmentation/reassembly, timeout, malformed frame protection;
- command authorization and capability advertisement;
- choose coexistence policy:
  - BLE and Wi-Fi simultaneous only if memory/radio proof passes;
  - otherwise explicit user-selected mode with clean transitions;
- no BLE feature toggle until runtime and UI are functional;
- companion-provided time/location import is explicit and user-controlled;
- fuzz frame decoder and disconnect races.

**Acceptance**

- official-compatible mobile/client connection;
- Public/channel/DM/contact synchronization as declared;
- 100 connect/disconnect cycles;
- four-hour BLE active soak;
- no heap leak, boot loop, or RF regression;
- malformed client cannot crash or exceed buffers.

**Artifact:** `ble_companion_acceptance_<sha>_<d1l>.json`

---

### WP-21 — Notifications, emoji, accessibility, and polish

**Issues:** #74, #76, new UX issue  
**Depends on:** WP-14, WP-15  
**Lane:** UI

**Implement**

- one unread/notification service for channels, DMs, admin replies, and system faults;
- visual badges and optional backlight pulse; do not claim audio unless hardware supports it;
- quiet-hours and privacy behavior;
- notification dedupe and clear semantics;
- curated embedded emoji/glyph subset with font-size budget;
- UTF-8 validation and replacement fallback;
- high contrast, night mode, text scaling within tested limits;
- consistent empty/loading/error/degraded states;
- focus and keyboard dismissal rules;
- no text baked into icons where localization is expected.

**Acceptance**

- notification count matches read cursors;
- duplicate packet does not duplicate notification;
- UTF-8/emoji fuzz and 138-byte boundary;
- accessibility screenshots and physical readability review;
- flash/PSRAM budget remains within gate.

**Artifact:** `ui_polish_acceptance_<sha>_<d1l>.json`

---

### WP-22 — Signed SD/OTA update and recovery

**Issues:** #21, #71, new partition-migration issue  
**Depends on:** WP-03, WP-11, WP-12, WP-17  
**Lane:** Update/security

**Stage 1: architecture decision**

- record current firmware size and future growth budget;
- design candidate partition tables;
- decide:
  - dual-slot OTA with a one-time USB partition migration; or
  - separately audited SD-staged recovery/bootloader design;
- prove retained NVS and settings locations across migration;
- define minimum bootloader/version compatibility;
- explicitly rule out unsafe in-app partition-table replacement.

**Stage 2: package security**

- signed manifest:
  - product/target;
  - version;
  - source SHA;
  - partition-table hash;
  - image SHA-256;
  - minimum/current version;
  - signer key ID;
- signature verification before any write;
- downgrade/anti-rollback policy;
- no network URL from untrusted packet/admin input;
- update source:
  - SD file selected locally;
  - optional HTTPS release source later;
- progress and cancellation only before irreversible stage.

**Stage 3: rollback/recovery**

- boot confirmation;
- automatic rollback after failed boot;
- recovery mode accessible without working UI;
- power-loss injection during download, verify, write, boot switch, and confirmation;
- preserve settings/identity/retained data;
- documented USB rescue.

**Acceptance**

- valid update, invalid signature, wrong target, corrupt/truncated image, low power, full storage, power loss, failed boot, rollback, downgrade rule;
- upgrade from last public release;
- clean factory/recovery image;
- update cannot be triggered silently over RF.

**Artifacts**

- `partition_update_decision_<sha>.md`
- `update_powerloss_acceptance_<sha>_<d1l>.json`
- `upgrade_matrix_<sha>_<d1l>.json`

---

### WP-23 — Optional observer/MQTT and companion location decision

**Issues:** new product decision  
**Depends on:** WP-13, WP-20  
**Lane:** Connectivity/privacy

The current settings model exposes observer intent without a release-ready observer runtime. Resolve it explicitly:

**Option A — defer/hide**

- remove or hide no-op observer controls;
- keep schema reservation;
- document not supported.

**Option B — implement**

- explicit opt-in;
- TLS and broker identity;
- privacy screen describing uploaded data;
- bounded queue, offline backoff, no RF forwarding;
- credentials redacted;
- no impact on Mesh runtime timing;
- data schema/version and user-visible connection state.

Companion location:

- manual location remains default;
- companion may set current location with source/time/accuracy;
- user can clear/disable;
- never claim D1L onboard GPS.

**Acceptance:** a recorded decision is mandatory; no no-op setting remains presented as functional.

---

### WP-24 — Final release qualification

**Issues:** #4, #7, #8, #11, #17, #23, #63, #69, #71, #78 and all remaining blockers  
**Depends on:** every work package in the selected release gate  
**Lane:** Release

**Code freeze conditions**

- zero open P0;
- zero known crash/data-loss/security P1;
- all declared features either working or hidden/clearly excluded;
- dependency/partition/capability manifests frozen;
- docs and ledger match source.

**Final exact-commit matrix**

1. two clean Actions runs from the same source SHA;
2. package/checksum/SBOM/provenance verification;
3. clean flash and preserve-settings upgrade;
4. cold/warm boot and 25 power cycles;
5. all primary navigation and 1,000 transition runtime test;
6. compose/scroll/pixel/manual physical review;
7. full FAT32/no-card/non-FAT32/multi-card/remove/reinsert/power-loss matrix;
8. Wi-Fi AP loss/reconnect and live Map matrix;
9. Public test-channel and targeted two-radio DM/ACK/PATH/trace;
10. authenticated admin matrix;
11. BLE matrix where included;
12. signed update/rollback matrix where included;
13. 12-hour idle/listening soak;
14. four-hour active mixed workload soak;
15. post-soak crash/heap/stack/queue/NVS/SD report;
16. final release-gate audit with exact evidence;
17. install/upgrade/recovery instructions followed by someone other than the implementer.

**Hard pass criteria**

- zero PANIC, WDT, unplanned brownout, boot loop, or LVGL corruption;
- zero unbounded queue growth or dropped critical event;
- zero user-data loss;
- zero incorrect delivery success;
- zero Public RF outside allowed test;
- zero SD formatting path;
- zero forbidden port use;
- no secrets in package/evidence/logs;
- all evidence references the exact tag commit.

**Artifacts**

- `release_gate_<sha>.json`
- `release_validation_report_<sha>.md`
- `release_evidence_index_<sha>.json`
- `known_limitations_<version>.md`

---

### WP-25 — Tag, publish, and post-release control

**Depends on:** WP-24  
**Lane:** Release

- tag the exact qualified commit;
- publish package, checksums, SBOM, provenance, notices, release report;
- mark supported hardware/SD/card size/SDK/upstream versions;
- publish known limitations without minimizing them;
- lock release branch;
- create patch-branch policy;
- capture user support template with diagnostics export instructions;
- schedule a 7-day issue review and first patch triage;
- do not immediately merge large feature work into the release branch.

**Completion condition:** users can install, verify, use, diagnose, upgrade, and recover the firmware from documented public artifacts.

---

## 13. Dependency and parallel-work plan

### 13.1 Critical path

```text
WP-01 SD/reboot proof
  → WP-02 land stack
    → WP-04 upstream oracle
      → WP-05 full conformance
        → WP-07 DM
          → WP-10 PATH/trace
            → WP-18 admin
              → WP-24 release

WP-02
  → WP-06 runtime owner
    → WP-07 / WP-08 / WP-09
      → WP-14 UI extraction
        → WP-15 / WP-16 / WP-17
          → WP-24

WP-01 + WP-06
  → WP-11 retained durability
    → WP-12 time
      → WP-13 Wi-Fi
        → WP-19 Map
          → WP-24

WP-03 + core contracts
  → WP-20 BLE
  → WP-22 update
  → full-feature WP-24
```

### 13.2 Safe parallel lanes

- **Lane A — Platform/storage:** WP-01, WP-11, WP-12.
- **Lane B — Protocol:** WP-04, WP-05, WP-07–WP-10, WP-18.
- **Lane C — UI:** simulator/view-model preparation can begin after WP-06 contracts, but must not invent protocol state.
- **Lane D — Release/security:** WP-00 and WP-03 can proceed in parallel after the integration baseline is known.
- **Lane E — Hardware:** only at named checkpoints; do not repeatedly flash for every docs/test-only change.

### 13.3 What must not run in parallel

- RP2040/retained structural changes while WP-01 exact-pair evidence is being qualified.
- UI implementation of DM/admin states before their application contracts are frozen.
- OTA partition work mixed with normal retained-store migration.
- BLE and Wi-Fi coexistence tuning before the single-owner runtime and memory telemetry exist.
- multiple PRs editing `ui_phase1.c`, `meshcore_service.c`, or the partition table simultaneously.

---

## 14. Hardware-test economy matrix

| Change type | Host/CI requirement | Physical requirement |
|---|---|---|
| Docs/ledger only | validators | none |
| Pure parser/codec | unit + sanitizer + upstream vectors | none until feature complete |
| UI layout/view model only | simulator, overflow, target, lifetime tests | one batched physical review at screen milestone |
| Mesh runtime/session | state-machine + conformance | one targeted two-radio gate after host closure |
| Wi-Fi/Map | mocks + memory contracts | current-SDK network/Map mini-gate |
| Retained model only | fault injection + schema tests | focused reboot/power-loss gate |
| RP2040/SD bridge | Arduino build + protocol tests | COM16/SD matrix |
| Partition/update | package/partition tests | dedicated migration/update/rollback gate |
| Final RC | full CI | full exact-commit matrix |

Every hardware script must:

- require explicit ports;
- reject forbidden ports;
- verify package/source/checksum;
- set DTR/RTS safely;
- record before/after reset/crash/health state;
- record `public_rf_tx` and `formats_sd`;
- restore the device to a known state;
- never erase NVS unless the specific approved recovery work package requires it.

---

## 15. Release gates

### G0 — Repository lineage

- active work based on current `main`;
- no hidden unique work on stale branch;
- ledger valid;
- clean PR dependencies.

### G1 — Reproducible build

- immutable actions/toolchain;
- all host/conformance/build/package/checksum jobs green;
- SBOM/provenance generated.

### G2 — Platform boot

- exact artifact;
- board/display/touch/radio/RP2040 health;
- supported SDK;
- no boot loop or crash.

### G3 — Storage/durability

- FAT32/no-card/non-FAT32;
- remove/reinsert;
- retained reconcile;
- power loss;
- NVS fallback;
- no format.

### G4 — MeshCore conformance

- full semantic bidirectional vectors;
- fuzz/sanitizers;
- declared packet surface complete.

### G5 — Messaging/routing

- Public/channels;
- bidirectional DM ACK/retry;
- direct/flood fallback;
- real PATH/trace;
- no duplicate/replay defect.

### G6 — UI safety and workflow completeness

- modular ownership;
- primary flows;
- keyboard/scroll/accessibility;
- physical review;
- 1,000 navigation transitions.

### G7 — Connectivity/Map/BLE

- Wi-Fi reconnect;
- live Map/cache/cancel;
- BLE if included;
- memory/coexistence soak.

### G8 — Admin/security/update

- authenticated admin;
- secret redaction;
- signed update/rollback if included;
- threat model and recovery docs.

### G9 — Soak and field readiness

- 12-hour idle/listening;
- four-hour active mixed workload;
- zero crash/data loss;
- support diagnostics and install test.

### G10 — Exact tag

- all evidence matches one SHA;
- release audit green;
- tag and package immutable.

---

## 16. Quantitative quality targets

These are release gates, not aspirations.

### Stability

- 0 PANIC/WDT/unplanned reboot in final matrix.
- 0 LVGL invalid-object/lifetime error.
- 1,000 primary-navigation transitions.
- 12-hour idle/listening soak.
- 4-hour active mixed workload soak.
- all task stack watermarks retain at least 25% headroom or 1 KiB, whichever is larger, unless a reviewed exception is documented.

### Memory

- no failed allocation;
- no monotonic heap/PSRAM loss across repeated screen, Map, Wi-Fi, BLE, and admin cycles;
- final RC baseline recorded per major screen/state;
- CI/hardware fail on a material unexplained regression.

### Messaging

- 100% expected ACK correlation in controlled success cases;
- lost ACK produces bounded retry/final state;
- duplicate inbound creates one visible row;
- absent peer never reports delivered;
- 138-byte boundary enforced before RF.

### Storage

- 0 format/erase command for SD;
- 0 accepted corrupt envelope;
- old-valid or new-valid state after every injected interruption;
- zero false card removal in inserted-card stability test;
- deterministic fallback and reconcile on true removal/reinsert;
- write amplification reported and bounded by coalescing policy.

### Map

- ≤9 unique tile requests per user-visible generation;
- no background/off-screen/multi-zoom prefetch;
- cache revisit causes 0 new request while reusable;
- attribution always visible;
- explicit User-Agent;
- cancellation completes within the configured network/read deadline.

### Security

- no password/private key/channel secret/admin credential in logs, screenshots, crash dumps, normal exports, or CI artifacts;
- third-party Actions pinned by SHA;
- update image signature and target verified;
- remote mutating admin requires authenticated session and local confirmation.

---

## 17. Product decisions Codex must record, not guess silently

1. Is authenticated repeater/room administration mandatory for `v1.0.0`, or may it move to `v1.1.0`? Current issue/docs say mandatory; default to mandatory until changed.
2. How many user channels are supported within RAM/flash budget?
3. What is the persistent pending-DM reboot policy: resume one bounded retry or mark interrupted and require user retry?
4. Timezone UX is decided: fixed UTC offsets from UTC-12:00 through UTC+14:00, including minute offsets; default UTC; no IANA database or automatic DST; protocol/certificate clocks remain UTC.
5. Is simultaneous Wi-Fi+BLE required, or is explicit mode switching acceptable?
6. Which Map provider strategy is acceptable for public scale?
7. Which update architecture is chosen after real binary-size analysis?
8. Is observer/MQTT in scope or hidden?
9. Which remote admin commands are read-only vs mutating vs destructive?
10. What curated emoji/glyph set fits the release image budget?

Where no answer is available, Codex must choose the safest reversible default, document it in an ADR, and continue.

---

## 18. Recommended issue/PR discipline

- one primary work package per PR;
- a PR description must list:
  - work-package ID;
  - issue;
  - exact baseline;
  - behavior changed;
  - behavior intentionally unchanged;
  - tests;
  - required hardware gate;
  - evidence filenames;
  - risk/rollback;
- no “drive-by” UI cleanup in storage/protocol PRs;
- no new feature in a bug-fix PR;
- no merge with a stale base or failing required check;
- no merge of physical behavior before exact artifact proof;
- prefer a sequence of small compatibility-preserving extractions over file-wide rewrites;
- update ledger, docs, test matrix, and known limitations in the same PR;
- close issues only after their stated closure artifact is valid.

---

## 19. Immediate next actions

1. **Do not start another feature branch from PR #80.**
2. Verify the newest exact ESP32/RP2040 artifacts and run WP-01.
3. If WP-01 fails, fix only the reproduced storage/reboot defect and repeat.
4. Land PRs #62, #64, and #80 in order.
5. Remove COM11 defaults from release/RF automation immediately after the baseline lands.
6. Add the completion ledger and immutable CI inputs.
7. Start WP-04 and WP-06 in parallel.
8. Complete host semantic conformance before the next full RF gate.
9. Build DM state machine before redesigning DM UI.
10. Continue through the dependency graph until both Stable Core and Full Feature gates are tagged.

---

## 20. Source/evidence index

### Current SIGUI anchors

- `README.md` at `07322ed...`
- `docs/ROADMAP.md`
- `docs/KRABSTHOUGHTS.MD`
- `docs/KNOWN_LIMITATIONS.md`
- `.github/workflows/d1l-ci.yml`
- `dependencies.lock`
- `sdkconfig.defaults`
- `partitions_d1l.csv`
- `main/app_main.c`
- `main/mesh/meshcore_service.c`
- `main/mesh/meshcore_wire.c`
- `main/storage/retained_blob_store.c`
- `main/storage/storage_status.c`
- `main/storage/map_tile_store.c`
- `main/map/map_view_service.c`
- `main/hal/rp2040_bridge.c`
- `firmware/rp2040_sd_bridge/deskos_sd_bridge/deskos_sd_bridge.ino`
- `main/comms/connectivity_manager.c`
- `main/comms/usb_console.c`
- `main/ui/ui_phase1.c`
- `scripts/release_gate_audit_d1l.py`

### Active issues

`#4, #6, #7, #8, #11–#23, #63, #65–#78`

### Pull requests

- PR #62 — bounded built-in Map
- PR #64 — ESP-IDF 5.5.4/platform integration
- PR #80 — conformance/durability/current SD repair

### Pinned upstream behavior

At MeshCore commit `e8d3c53...`:

- `src/helpers/BaseChatMesh.h`
- `src/helpers/BaseChatMesh.cpp`
- `examples/companion_radio/MyMesh.cpp`
- `docs/payloads.md`

The upstream helper contains the expected ACK hash/session, direct-vs-flood, ACK+PATH, contact, channel, login/request, and timeout concepts that SIGUI must either reuse or prove byte-for-byte through the oracle.

### External policies

- OpenStreetMap Tile Usage Policy: current-view interactive access, visible attribution, identifying User-Agent, caching, and no bulk/offline prefetch.
- GitHub Actions secure-use guidance: full-length action SHAs are the immutable pinning mechanism.
- ESP-IDF v5.5.4 release and programming guide.

---

## 21. Final instruction to the implementation agent

Do not optimize for the number of commits, the number of closed issues, or the speed at which UI screenshots change. Optimize for **verified behavior per unit of change**. Bank evidence, reduce ownership ambiguity, keep physical testing issue-specific, and never let a historical green receipt satisfy a new SHA.

The work is complete only when the exact tagged source, package, hardware behavior, protocol behavior, retained data, update/recovery path, and public documentation all agree.
