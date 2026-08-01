HISTORICAL RECORD — DO NOT EXECUTE

This document predates the RC1 authority reset. It is retained only for
provenance. It cannot create work, tests, evidence requirements, or release
gates. See `docs/RC1_SCOPE.md` and `docs/ROADMAP.md`.

<!-- Historical output from scripts/completion_ledger.py; edit only the archived ledger. -->
# SIGUI Completion Status

> Historical generated ledger snapshot. It is retained for audit history, not as RC1 release authority. Use `README.md`, `docs/USER_GUIDE_D1L.md`, `docs/TEST_PLAN_D1L.md`, and `docs/RELEASE_CHECKLIST.md` for the current `core_1_0` / `conditional` release.

Snapshot: `2026-07-18T16:23:32Z`

- Observed live `main`: `bd6ea0e685442d8a820766f4686395e50ca5397f`
- Last strict-banked `main`: `bd6ea0e685442d8a820766f4686395e50ca5397f`
- Working branch: `codex/post-pr188-ledger` from `bd6ea0e685442d8a820766f4686395e50ca5397f`
- Integrated candidate: `bd6ea0e685442d8a820766f4686395e50ca5397f`
- Pinned MeshCore: `e8d3c53ba1ea863937081cd0caad759b832f3028`
- Selected ESP-IDF: `5.5.4`
- Release posture: **not ready to tag**
- Ledger release ready: **no**
- Highest-priority pending: `WP-02`
- Highest-priority runnable now: `WP-04`

## Progress estimate

- Capability implementation: **80%** (`(7*100 + 13*90 + 5*60 + 2*0) / 27`)
- Applicable exact-main automated acceptance: **0%**
- Final release-gate closure: **0%** (0 of 11 green)
- Weighted full-release progress: **64%** (`0.8*80.3703703704 + 0.1*0 + 0.1*0`)
- Implementation-complete (7): Boot / board / display / touch, Home/navigation, Public messages, Packets, Radio profile, SD bridge/core files, Diagnostics/crash/health
- Advanced (13): Direct messages, Contacts, Heard nodes, Channels, Routes, Trace, Identity/adverts, Retained history, Map, Wi-Fi, Emoji/UTF-8, QR sharing, Packaging/release
- Partial (5): Repeater/room browser and administration, Notifications, Terminal/log view, Local GPS/own location, Observer/MQTT
- Not started (2): BLE companion transport, Signed SD/OTA update and recovery

This estimate is a repeatable reporting aid, not a release-gate waiver. The authoritative release decision remains fail-closed until every applicable final gate is green.

## Work packages

| Package | Status | Dependency gate | Proof banked | Blockers |
|---|---|---|---:|---|
| `WP-01` Bank PR #80 exact-pair storage/reboot gate | `merged` | `proof_banked` | yes | — |
| `WP-02` Land and normalize PR stack | `in_progress` | `implementation_merged` | no | BLK-WP02-EXACT-HARDWARE-ROLES-20260713 |
| `WP-00` Live completion ledger | `merged` | `merged` | no | — |
| `WP-03` Immutable CI and release inputs | `merged` | `merged` | yes | — |
| `WP-04` MeshCore boundary and upstream oracle | `in_progress` | `implementation_merged` | no | BLK-WP04-IDENTITY-DISPATCH-20260713 |
| `WP-06` Single-owner Mesh runtime | `in_progress` | `merged` | no | — |
| `WP-05` Semantic conformance and fuzzing | `in_progress` | `merged` | no | — |
| `WP-07` Truthful DM ACK/retry/delivery | `blocked` | `merged` | no | — |
| `WP-08` Canonical contacts/heard nodes/QR lifecycle | `blocked` | `merged` | no | — |
| `WP-09` Multi-channel model and messaging | `blocked` | `merged` | no | — |
| `WP-10` Real PATH/direct fallback/trace | `blocked` | `merged` | no | — |
| `WP-11` Retained durability/schema/reset/power loss | `blocked` | `merged` | no | — |
| `WP-12` Truthful time service | `blocked` | `merged` | no | — |
| `WP-13` Wi-Fi reconnect/credentials/safe mode | `blocked` | `merged` | no | — |
| `WP-14` UI ownership and modular extraction | `blocked` | `implementation_merged` | no | — |
| `WP-15` Finish Messages UX | `blocked` | `merged` | no | — |
| `WP-16` Finish Nodes/Network/roles/Map pins | `blocked` | `merged` | no | — |
| `WP-17` Packets/diagnostics/Terminal | `blocked` | `merged` | no | — |
| `WP-18` Authenticated repeater/room administration | `blocked` | `merged` | no | — |
| `WP-19` Map provider/cache lifecycle | `blocked` | `merged` | no | — |
| `WP-20` BLE companion transport | `blocked` | `merged` | no | — |
| `WP-21` Notifications/emoji/accessibility/polish | `blocked` | `merged` | no | — |
| `WP-22` Signed SD/OTA update and recovery | `blocked` | `merged` | no | — |
| `WP-23` Observer/MQTT and companion-location decision | `blocked` | `merged` | no | — |
| `WP-24` Final release qualification | `blocked` | `merged` | no | — |
| `WP-25` Tag/publish/post-release control | `blocked` | `merged` | no | — |

## WP-01 exact-candidate checkpoint

- Merged implementation SHA: `4ee07caf09906abdcebe8faccd95790dceb5fe88`
- Exact physical evidence SHA: `092293f2311a24c9899bc9bf343ab014c4ba0411`
- Execution state: **merged** (stage `hardware_green`)
- Host suite: **pass**, 773 tests
- Actions: push `29272708844` and PR `29272709642` are **success**
- Downloaded checksums: **verified** across 8 manifests
- Exact COM12/COM16 pair installed: **yes**
- Partial physical passes: clean autonomous READY_SD baseline, inserted-card stability for 1201.171 seconds, 10 of 10 real removal/reinsert cycles, 5 of 5 controlled retained reboot cycles, 7207.089 seconds across six storage-active segments, minimum observed stack floor 7976 bytes, zero unexpected backend-generation changes
- Physical gate: **pass** — no failure recorded
- WDT/PANIC observed: **no**
- Safety flags: `public_rf_tx=false`, `formats_sd=false`
- Physical proof banked: **yes**
- Merge state: **merged**

`WP-02` is unlocked because WP-01 banked its required exact-pair hardware proof. The implementation may land on a successor SHA while the evidence remains explicitly bound to its proven source SHA.

## Open blocker receipts

| Receipt | Work package | Missing input | Required action |
|---|---|---|---|
| `BLK-WP02-EXACT-HARDWARE-ROLES-20260713` | `WP-02` | Five exact frozen-candidate role receipts: board boot/runtime on COM12, UI workflow on COM12, SD path on COM12/COM16, retained reboot on COM12/COM16, and Map-open on COM12. The tracked 4ee07caf baseline proves the merged stack and fails closed on these roles; they are intentionally deferred until the release-security/protocol slices stop moving the candidate SHA. | No device action now. After the final candidate is frozen, install its checksum-verified exact ESP32/RP2040 pair and collect only the five narrow role receipts on COM12/COM16. Do not use forbidden ports, transmit Public or DM RF, or format SD. |
| `BLK-WP04-IDENTITY-DISPATCH-20260713` | `WP-04` | Complete retained contact lifecycle and recovery with truthful UI/sendability behavior; implement approved multi-byte or controlled multi-hop TRACE plus truthful UI; implement only pinned safe room administration and authorized mutations; and collect official-peer RF plus exact-candidate hardware proof. | Continue safe software work without device action; collect controlled-peer RF and exact-candidate hardware proof only after the release candidate is frozen and RF authorization is explicit. |

## Release gates

| Gate | Status |
|---|---|
| `G0` repository_lineage | `open` |
| `G1` reproducible_build | `blocked` |
| `G2` platform_boot | `blocked` |
| `G3` storage_durability | `blocked` |
| `G4` meshcore_conformance | `blocked` |
| `G5` messaging_routing | `blocked` |
| `G6` ui_safety_workflows | `blocked` |
| `G7` connectivity_map_ble | `blocked` |
| `G8` admin_security_update | `blocked` |
| `G9` soak_field_readiness | `blocked` |
| `G10` exact_tag | `blocked` |

## Safety invariants

- Firmware builds run only in GitHub Actions; downloaded artifacts require checksum verification.
- `D1L_PORT`, `RP2040_PORT`, and `MESH_PEER_PORT` have no operational defaults.
- Forbidden ports: COM8, COM11, COM29.
- SD cards are never formatted on-device.
- Simulation, dry-run, source review, and predecessor-SHA evidence cannot close a physical gate.
