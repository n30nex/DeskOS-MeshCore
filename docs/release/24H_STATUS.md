# MeshCore DeskOS D1L Full Feature — Production Status

> **SUPERSEDED HISTORICAL EVIDENCE LEDGER — NOT RELEASE AUTHORITY.**
> This file preserves earlier Full Feature/minimal-Core checkpoints. Its
> profile, capability, COM-port, SD/NVS, recovery and soak statements are not
> current RC1 instructions and must not be used to build, flash, test or tag a
> release. Use the [project README](../../README.md), [RC1 user guide](../USER_GUIDE_D1L.md),
> [RC1 test plan](../TEST_PLAN_D1L.md), and
> [SD-card guide](../D1L_SD_CARD_GUIDED_INSTALL.md).

## Current continuation (2026-07-25)

| Field | Current value |
|---|---|
| Repository | `n30nex/SIGUI` |
| Integration branch | `release/24h-core` |
| Integration worktree | `F:\SIGUI-worktrees\release-24h-core` |
| Release profile | `full_feature` |
| SD history mode | `conditional` |
| Feature implementation | `100%` |
| Public release readiness | `false` pending exact candidate evidence |
| Candidate SHA | pending final commit |
| Candidate Actions run | pending exact-SHA run |
| Current D1L host | Pi 5 `neopi5` / `192.168.0.24` |
| Development account | `siguidev`, unprivileged and key-only |
| Authorized selector | `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0` |
| Required USB identity | `1a86:7523` |
| Controlled-peer access | still requires narrow explicit authorization |

All user-facing domains are now implemented: full navigation/messaging,
multi-channel/contact/QR management, Nodes/Packets/Map/TRACE, Wi-Fi, bonded
BLE companion core protocol, authenticated repeater/room administration,
TLS/QoS1 Observer, conditional SD history, Ed25519-signed dual-slot update,
display/accessibility, notifications, structured terminal/event log,
diagnostics and recovery.

Release closure now has one straight-line sequence: source gates → immutable
commit → exact-SHA Actions package → checksum verification → exact Pi 5 flash
→ automated device/reboot/storage/controlled-peer acceptance → one
consolidated physical UI confirmation → final audit. No predecessor evidence
is transferred to the candidate.

Immutable current rules:

- use only the stable Pi by-id selector after proving `1a86:7523`;
- never probe another Pi serial device and never substitute a raw tty;
- stale Windows COM assignments are historical only;
- never format SD;
- never automate default Public RF;
- never disclose or export identity/private/update signing secrets;
- keep `full_feature_release_ready=false` until every exact-candidate receipt
  exists and passes.

## Historical Core sprint ledger

The remainder of this file is the preserved 2026-07-18 through 2026-07-24
Core sprint ledger. Its profile, percentages, commits, runs and Windows routes
are historical, not the current candidate state.

The authoritative historical product boundary, roadmap, and work graph were:

- `SIGUI_CORE_1_0_PRODUCT_CONTRACT_2026-07-18.md`
- `SIGUI_24H_AUDIT_AND_ROADMAP_2026-07-18.md`
- `SIGUI_24H_EXECUTION_BACKLOG_2026-07-18.yaml`

## Sprint identity

| Field | Value |
|---|---|
| Sprint start | `2026-07-18T13:38:38-04:00` |
| Hard elapsed deadline | `2026-07-19T13:38:38-04:00` |
| Repository | `n30nex/SIGUI` |
| Integration branch | `release/24h-core` |
| Integration worktree | `F:\SIGUI-worktrees\release-24h-core` |
| Starting `origin/main` | `846f728dd3faded85451c6d39ba6a07cb8ca7f44` |
| Starting main Actions run | `29652673963` (`d1l-ci`, success) |
| Target tag | `v1.0.0` |
| Release profile | `core_1_0` |
| Core readiness | `false` |
| Full-feature readiness | `false` |
| Candidate SHA | pending replacement R15 freeze; `f0fef39eb37f5795ebd683338c11a37ff2fc8399` is superseded |
| Candidate Actions run | pending replacement workflow dispatch; run `29669268575`, attempt 1, succeeded only for superseded `f0fef39...` |
| SD history mode | `disabled` |
| Current D1L host | Raspberry Pi 5 `neopi5` |
| Development account | `siguidev`; unprivileged, key-only |
| Current D1L selector | `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0` |
| Expected USB identity | `1A86:7523` |
| Current raw resolution | `/dev/ttyUSB2`; observational only, never release identity |
| Windows D1L alternative | `COM12` |
| Controlled-peer access | pending narrowly scoped status/control access for `siguidev` |

## Immutable safety rules

- Firmware builds run only in GitHub Actions.
- Current D1L app/console/flash work on `neopi5` uses only
  `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0` after verifying
  `1A86:7523`, readability, writability, and symlink resolution.
- `/dev/ttyUSB2` is the current observation only; raw `/dev/ttyUSB*` names are
  never release identity.
- `COM12` remains the valid Windows D1L alternative.
- `COM16` is allowed only when separately authorized SD/RP2040 qualification
  requires it and is never the Core D1L target.
- `COM8`, `COM11`, and `COM29` are forbidden.
- Never format SD.
- Normal candidate flashing is non-erasing.
- Never automate transmission on the default Public RF channel.
- Predecessor, simulated, dry-run, source-only, or manually edited evidence cannot close an exact-candidate gate.
- Unsupported Core features must be unreachable and side-effect-free.
- Tagging is forbidden until the exact Core audit reports
  `core_release_ready=true`, all Core P0 gates pass, and no Core
  crash/data-loss/security P1 remains.

## Product decisions

- Core 1.0 supports only the capability matrix in the product contract.
- BLE is unavailable. PR #199 is explicitly excluded from this release.
- Map, user Wi-Fi control, multi-channel management, administration,
  Observer/MQTT, signed SD/OTA update, GPS/location, mutable terminal,
  advanced QR/emoji, and user-facing TRACE/PATH tools are unavailable.
- SD is disabled for this candidate because the required paired `COM16`
  SD/RP2040 target is absent and therefore cannot be qualified against the
  exact candidate. The RP2040 payload must be omitted and retained NVS is
  authoritative.
- PR #197 was integrated as reviewed `.c`/`.h`/native-test substance.
  Generated manifests were regenerated against this branch rather than
  accepting stale hashes from its conflicting branch.

## Current GitHub snapshot

Snapshot refreshed at `2026-07-23T18:18:10-04:00`.

| Item | State |
|---|---|
| `origin/main` | `846f728dd3faded85451c6d39ba6a07cb8ca7f44` |
| Main `d1l-ci` | run `29652673963`, success |
| PR #200 | open draft; the single Core integration PR; head `f0fef39eb37f5795ebd683338c11a37ff2fc8399` |
| PR #197 | open, non-draft; reviewed bounded substance already integrated; head `79d3efae9c784d65b70b0d79fae352b3ad5199aa` |
| PR #199 | open draft at `6c854159dfc37525ff4df2d57ca216e4b2303bc2`; excluded |
| Open release-blocker P0 issues | 21; all classified for Core 1.0 |
| Existing `v1.0.0` tag/release | none |
| Active/queued Actions runs at snapshot | none |

## P0 tracker classification

Each open P0 has exactly one Core-sprint classification label. Classification
does not close an issue or substitute for exact-candidate evidence.

| Classification | Issues | Core handling |
|---|---|---|
| `core-blocker` | #71 | Must close through exact package, Actions, hardware, audit, and release evidence |
| `evidence-only` | #7, #8, #63, #66, #67, #68, #69, #70, #74, #75, #76 | Software is present for the Core subset; exact candidate evidence remains fail-closed |
| `full-feature-deferred` | #4, #6, #11, #13, #14, #16, #73, #77, #78 | Unavailable Core surface; retained for Full Feature work |
| `stale/close after reconciliation` | none | Nothing was closed as stale during classification |

Mixed-scope issues are narrowed by the authoritative Core contract: #67
requires only basic contact-to-DM evidence while multi-channel breadth is
deferred; #68 requires internal DM PATH/route behavior while user-facing
TRACE is deferred; #74 is fixed Public plus DM only; and #76 omits unavailable
Map, Wi-Fi, BLE, and SD feature claims.

## File ownership

| Role | Work packages | Exclusive ownership |
|---|---|---|
| Lead/integrator | R0, R1, R9–R16 | integration branch; `main/app/release_profile.*`; freeze; Actions; hardware; release decision |
| Agent A — Core UI | R2 | `main/ui/**`, including sole ownership of `main/ui/ui_phase1.c` |
| Agent B — storage/persistence | R4, R5 | `main/storage/retained_blob_store.*`; coordinates profile fields with lead |
| Agent C — core Mesh/RF | R6 | `main/mesh/**`; sole implementation owner of `main/mesh/meshcore_service.c` |
| Agent D — command admission | R3 | `main/comms/usb_command_parser.*`, `main/comms/usb_console.*`; coordinates app-model edits with lead |
| Agent E — QA/package/audit | R7 | new Core runners/audit/tests and `scripts/package_release_d1l.py`; no firmware runtime files |
| Agent F — independent review | R8 | review notes, operator command sheet, receipt validation; no implementation by default |

No sub-agent may edit this ledger, completion ledgers, or release status.
The lead reviews and cherry-picks each bounded commit in dependency order.

## Work-package status

| ID | Status | Evidence / next action |
|---|---|---|
| R0 | complete | Contract/roadmap/backlog and ledger committed as `c09c9b4e6ee0a7eb9ea4bc405369ffdc94544265` |
| R1 | complete | Central immutable profile, compile-time disabled-SD mode, app/runtime projections, package scope, version, health, settings, Mesh, companion, and storage truth are integrated and tested |
| R2 | complete | Initial boundary `bec47e2` plus truth/profile-matrix follow-up `8b36d99`; 227 integrated UI/profile/map tests passed |
| R3 | complete | Profile-aware command admission integrated as `b356e45` plus disabled-SD fail-closed follow-up `cbbfbed`; 52 integrated command/profile/source-pin tests passed |
| R4 | complete | Reviewed PR #197 substance integrated as `2824d63c6c779560ce0ad1ca787e230634b5c3ff`; 47 storage tests and 7 integrated source-pin checks passed |
| R5 | complete | Profile-bound SD admission integrated as `0c72561`; Core conditional/disabled routes retained data to NVS and cannot activate SD |
| R6 | complete | Public-only Core runtime boundary `e3f99a6` rejects private-channel RX/TX before decrypt/key/store/RF and prevents retained private-channel reconciliation; focused Mesh/DM/contact/route suites are green |
| R7 | in progress | The f0 producers exposed retained pagination/snapshot safety and Pi5-peer binding gaps; bounded repairs and audit consumption are under review |
| R8 | in progress | Independent review rejected the first retained and Core-simulator repair commits; corrected commits require fresh review |
| R9 | in progress | The f0 host suite passed before its freeze, but runtime-affecting R15 repairs require focused validation and one new complete host suite |
| R10 | superseded / pending | Exact run `29669268575`, attempt 1, succeeded and all five archives verified for f0; dispatch a fresh `include_sd_bridge=false` run after replacement freeze |
| R11 | failed closed / pending | f0 flash, smoke, and Core UI passed, then retained seed failed; all physical gates must restart on the replacement candidate |
| R12 | pending | Controlled peer RF/DM; `public_rf_tx=false` |
| R13 | in progress | SD disabled/NVS fallback selected because COM16 is absent; exact package/device truth remains |
| R14 | pending | 60-minute active plus 30-minute idle exact-candidate soak |
| R15 | in progress | Repair retained full-store proof, Core simulator reachability, exact protocol-migration evidence, and Pi5 RF/soak peer binding; no rejected repair commit may be integrated |
| R16 | pending | Final Core audit and tag/release or exact no-go |

## Exact-candidate evidence ledger

All rows remain fail-closed until an exact receipt is recorded.

| Gate | Status | Exact evidence |
|---|---|---|
| Source/profile frozen | superseded / pending | f0 was clean and exact; rejected R15 repairs are not integrated and no replacement source is frozen |
| Full host suite | superseded pass / pending | The f0 tree passed before freeze; one replacement-candidate full host suite is required after integration |
| Final Actions workflow | superseded pass / pending | Run `29669268575`, attempt 1, succeeded only for superseded `f0fef39...`; exact replacement dispatch required |
| Artifact downloads/checksums | superseded pass / pending | All five f0 archives and embedded trees verified; a fresh exact replacement directory and checksum receipt are required |
| Profile/package/provenance/SBOM binding | superseded pass | Exact f0 run/package trees verified; new SHA/run binding required |
| Non-erasing COM12 flash | superseded bootstrap pass | `esp32_flash_bootstrap_f0fef39_actions_29669268575_attempt_1_COM12.json`; exact identity passed without erase, but `closure_eligible=false` |
| Core smoke/display/touch | superseded pass | Fresh exact-f0 Core smoke passed with disabled-SD/NVS truth and clean crashlog |
| 20-round Core UI probe | superseded pass | Exact-f0 UI ran 20 rounds / 120 events successfully; replacement-candidate proof is required |
| Compose/scroll surfaces | superseded physical pass | Exact-f0 Core UI checks passed; replacement-candidate proof is required |
| Five software reboots | missing | — |
| Three cold boots | missing | — |
| Retained state | exact failed / superseded | f0 seed receipt is immutable and `closure_eligible=false`; initial pagination/contact capture failed before mutation |
| Protocol timestamp migration | missing | f0 reports legacy lower-bound confirmation required; no migration attempted |
| Controlled RF/DM/ACK/PATH/direct route | missing | — |
| SD decision | superseded pass / selected disabled | f0 package/device confirmed disabled-SD/NVS; COM16 remains absent and the replacement must repeat this truth |
| 60-minute active soak | missing | — |
| 30-minute idle soak | missing | — |
| Install/recovery package review | missing | — |
| Final Core audit | missing | — |

## Release decision

Current decision: **NO-GO / execution in progress**.

`core_release_ready=false` and `full_feature_release_ready=false`. No tag or
release is authorized by the evidence currently recorded.

## Execution updates

### `2026-07-18T14:10:06-04:00`

- R0 froze the exact authority pack on the integration branch.
- R1 added the deterministic `core_1_0` CMake default, immutable feature
  authority, compile-time `conditional` SD mode, app-snapshot profile binding,
  and a four-case native profile/SD matrix.
- Focused R1 validation: 67 tests passed across release-profile, UI shell,
  storage hierarchy, diagnostics, release metadata, CI workflow, and Actions
  security contracts.
- Three isolated worktrees are active for R2, R4, and R6. No firmware build,
  serial port, RF transmission, or SD operation has occurred.

### `2026-07-18T14:20:43-04:00`

- R4 integrated the reviewed runtime/header substance from PR #197, added
  fail-safe regression coverage for NVS comparison-read failures, and
  regenerated its transitive manifest pins from the integrated tree.
- Integrator validation for R4 passed 47 retained/factory-reset/storage tests
  plus seven exact source-pin checks. Initial pin failures correctly exposed
  R1's `main/CMakeLists.txt` and app-model changes; the generated hashes were
  refreshed from current canonical-LF sources and then passed.
- R6 completed as a no-code result: 109 focused MeshCore, DM, ACK/retry,
  duplicate/replay, contact authorization, route/PATH, and runner-contract
  tests passed; one libFuzzer execution remains intentionally Actions-only.
- R6 found an exact-evidence gap outside its ownership:
  `rf_full_acceptance_d1l.py` accepts a commit argument without verifying the
  device build and permits a non-`COM12` D1L port. R12 remains fail-closed;
  the QA/package slice must add exact device-version and `COM12` admission.
- R3 command admission and R5 conditional-SD/NVS fallback are now active in
  fresh worktrees based on the integrated profile. No firmware build, serial
  port, RF transmission, or SD operation has occurred.

### `2026-07-18T14:41:48-04:00`

- R5 integrated a release-profile admission check at the retained-store
  boundary. In Core `conditional` or `disabled` mode, even a fully capable
  bridge cannot claim or activate SD; retained reads, writes, and erases route
  to NVS. `supported_optional` still requires every runtime prerequisite.
- R2 integrated the exact Core dock (`Home`, `Messages`, `Nodes`, `Packets`,
  `Settings`), Public-only channel projection, unavailable-screen/deep-link
  rejection, NVS-only storage wording, and controller/timer admission gates
  for excluded features.
- Integrated R2 validation passed all 212 `test_ui_*` tests and 19 focused
  release-profile, retained-store, and map-marker source-pin tests. The two
  UI source hashes used by the map-marker oracle were regenerated from the
  integrated files.
- Read-only hardware preflight now sees `USB-SERIAL CH340 (COM12)` present and
  OK. `COM16` is absent. No serial port has been opened, no firmware has been
  flashed, no RF transmission has occurred, and no SD operation has occurred.
- Independent R2 review found no crash, null-controller, or excluded-timer
  blocker, but held completion for four contract-truth cleanups: remove a Core
  room-conversation claim, avoid claiming a notification system, omit Map
  storage rows when Map is unavailable, and avoid inherited SD warning styling
  in NVS-only mode. The UI owner is implementing a bounded follow-up with
  stronger Core-compiled controller coverage.

### `2026-07-18T14:53:45-04:00`

- R3 integrated an ordered profile-aware USB command allow/deny table before
  command handlers, the exact `ESP_ERR_NOT_SUPPORTED`/profile/feature response,
  truthful read-only unavailable-feature status, and release profile plus SD
  mode fields in version, health, settings, mesh, companion, and storage
  output.
- A lead review follow-up added disabled-SD admission for media and RP2040
  mutations while retaining conditional-mode qualification and bounded
  read-only status/diagnostic probes. Core help no longer offers
  `storage force-nvs off`.
- The console's MeshCore-oracle and USB-parser canonical-LF SHA-256 pins were
  regenerated from the integrated source. Integrated validation passed 32
  command/profile/connectivity/channel/storage tests plus 20 USB-parser/oracle
  tests; one documented Actions-only oracle execution was skipped.
- R7 Core smoke, Core UI corruption probe, separate Core audit, package
  capability truth, and exact COM12/firmware admission are active in an
  isolated worktree. No build, serial-port open, hardware mutation, RF
  transmission, or SD operation occurred.

### `2026-07-18T15:03:31-04:00`

- GitHub state was refreshed again before candidate preparation:
  `origin/main` remains
  `846f728dd3faded85451c6d39ba6a07cb8ca7f44`, PR #197 and excluded draft
  PR #199 remain the only open pull requests, and main `d1l-ci` run
  `29652673963` remains successful.
- Read-only PnP preflight still reports only the allowed D1L endpoint,
  `USB-SERIAL CH340 (COM12)`, present and OK. The paired `COM16` SD/RP2040
  target is absent.
- The candidate SD decision is therefore fail-closed: compile-time SD history
  defaults to `disabled`, retained NVS is authoritative, and R7 packaging must
  omit the RP2040 payload. Exact package and flashed-device truth remain
  required before R13 closes.
- No serial port has been opened, no firmware has been flashed, no RF
  transmission has occurred, and no SD operation has occurred.

### `2026-07-18T15:06:46-04:00`

- The independent R8 review found a candidate-freeze blocker: `version`
  exposed the exact 40-hex build commit, but `health` exposed only release
  profile and SD mode. R7 intentionally requires both preflight commands to
  bind to the exact candidate before any hardware action.
- The shared profile/status field emitter now includes
  `D1L_BUILD_GIT_COMMIT`, binding `health` and other profile-aware status
  results to the exact Actions source. The USB parser and MeshCore oracle
  canonical-LF source pins were refreshed to
  `87362a8d7a3591a2fc66ca31d8923f3e52ee48b2e7e185ec78aad49d81c23ff6`.
- Focused lead validation passed 51 release-profile, command-admission,
  diagnostics, source-pin, retained-profile, metadata, workflow, and Actions
  security tests; one documented Actions-only execution was skipped.
- The operator power-cycled the present device. Read-only PnP observation
  confirmed that COM12 returned present and OK. This is a pre-candidate
  hardware check and cannot count toward the exact-candidate cold-boot gate.
- No serial port was opened, no firmware was flashed, no RF transmission
  occurred, and no SD operation occurred.

### `2026-07-18T15:11:31-04:00`

- R2 follow-up `8b36d99` integrated accurate Core Home and unread-counter
  wording, Map-location filtering when Map is unavailable, neutral NVS-only
  storage presentation with real NVS faults preserved, centralized Public
  channel projection, and Core/development native matrices.
- The map-marker oracle pins for `ui_node_detail.c` and `ui_phase1.c` were
  regenerated from the integrated canonical-LF sources. One legacy source
  assertion was updated to require the new profile projector plus projected
  channel lookup instead of the removed inline expression.
- Integrated R2 validation passed all 227 UI, release-profile,
  retained-profile, and map-marker tests.

### `2026-07-18T15:15:36-04:00`

- `origin/main`, open PRs, and Actions were refreshed again. Main remains
  `846f728dd3faded85451c6d39ba6a07cb8ca7f44`; PR #197 and excluded draft
  PR #199 remain the only open PRs; no workflow was triggered by the
  `release/24h-core` backup push.
- All 21 open P0 issues were reviewed against their current bodies and given
  exactly one required classification label: one `core-blocker`, eleven
  `evidence-only`, nine `full-feature-deferred`, and zero stale closures.
  No issue was closed, and evidence-only items remain open until the exact
  candidate receipts pass.

### `2026-07-18T15:25:58-04:00`

- Independent documentation review confirmed that the previous public README
  and user guide advertised Full Feature Map, Wi-Fi, BLE, SD, channel,
  administration, TRACE, and forbidden-port workflows that are unreachable in
  Core 1.0.
- The public README and documentation index now describe only the immutable
  Core surface, SD-disabled/NVS operation, Actions-only builds, COM12, exact
  package verification, and unavailable capabilities. The prior full-feature
  user guide is preserved under an explicit historical filename and replaced
  by a Core-only user guide. Flash/recovery guidance now uses COM12 and
  distinguishes non-erasing normal flash from typed-confirm destructive
  recovery.
- Focused documentation, storage, location-boundary, DM-identity, no-format,
  and hardware-script validation passed 79 tests.
- R7 is independently removing the Full Feature and RP2040/SD documents from
  Core packages and generating exact Core install/recovery, support-matrix,
  and release README files. Final package review remains open until that
  commit is integrated.

### `2026-07-18T15:35:50-04:00`

- Independent command/documentation review found two incorrect Core console
  spellings and unsafe repository-relative install/recovery examples. The
  Core guide now uses the implemented `board` and `identity status` commands.
  Install/recovery guidance permits only the checksum-verified, package-root
  non-erasing `flash_project.ps1` on COM12 for a normal install; the separate
  full-flash helper is destructive-recovery-only and typed-confirmed.
- Focused package, documentation, and release-profile validation passed 32
  tests with one documented Actions-only skip.
- R8 also found that the package flash helper did not produce the exact flash
  receipt required by the Core audit, and that the first R7 audit draft did
  not verify raw reboot receipts or uninterrupted boot continuity across the
  active-to-idle soak seam. R7 remains open while adding bounded fail-closed
  evidence paths and tests; no candidate has been frozen.
- Read-only PnP metadata identifies the separate present COM15 endpoint as an
  `NRF52 DK` TinyUSB serial device. It is only a potential controlled RF peer:
  the port has not been opened and cannot be admitted until the exact Core RF
  runner validates its identity and policy. COM12 also remains unopened.
- No firmware was built locally, no serial port was opened, no firmware was
  flashed, no RF transmission occurred, and no SD operation occurred.

### `2026-07-18T16:04:58-04:00`

- The controlled peer on COM15 was identified through its already-running
  OpenClaw listener status rather than by taking over the port. The listener
  reports `serial.port=COM15`, `mesh_connected=true`, public-key prefix
  `024999dedfd2`, and deterministic direct-message handling: a DM containing
  `test` receives `Test OK DM.`. A bounded direct open attempt was denied
  before the port opened because the listener owns it; no RF action occurred.
- The release workflow now writes
  `d1l-host-artifacts/build-inputs/d1l-candidate-scope.json` before the exact
  host build-input checksum manifest. The receipt binds the source commit,
  run ID and attempt, canonical repository/workflow, manual-dispatch event,
  ESP32-only scope, `release_profile=core_1_0`, and disabled SD history. A
  manual false dispatch cannot be changed by last-commit path inference.
- The host workflow also runs the separate Core audit in explicitly
  non-closing dry-run mode and passes explicit Core/disabled arguments to the
  package generator. Focused workflow validation passed all eight contract
  tests. Commit `8901b42` is pushed; branch pushes do not trigger `d1l-ci`.
- R8 still holds candidate freeze for fail-closed QA gaps under active repair:
  executable COM15/OpenClaw RF adaptation, raw/manual/cold-cycle and operator
  receipt producers, package-path containment and license-notice checks, and
  the narrow pre-tag sequencing rule for release-tracking issue #71. No full
  host suite or final Actions candidate has run.
- COM12 remains present after the operator's pre-candidate power cycle. That
  cycle is not release evidence. No firmware was built locally, COM12 was not
  opened or flashed, no RF transmission occurred, and no SD operation
  occurred.

### `2026-07-18T16:30:47-04:00`

- Post-power-cycle read-only Windows checks confirm
  `USB-SERIAL CH340 (COM12)` is present with `CM_PROB_NONE` and remains mapped
  to the live serial namespace. `Win32_SerialPort` did not enumerate the CH340,
  so release admission uses the authoritative Plug-and-Play state. COM12 was
  not opened and the pre-candidate cycle remains ineligible for release proof.
- Commit `c84b190` adds executable Core-only manual UI and package
  install/recovery review producers. Both fail closed unless bound to the
  exact candidate SHA, Actions run and attempt, clean source, exact COM12/Core
  profile, and hashed automated/package evidence. They require distinct
  operator and reviewer attestations; optional PNG evidence is validated when
  supplied but is not an added Core-only prerequisite.
- Focused validation passed all nine manual UI and install/recovery producer
  tests plus Python compilation. The branch remains unfrozen while R7
  artifact/RF/flash work and R8 reboot/cold-cycle and live-defect producers
  complete. No full host suite or candidate workflow has run.
- No firmware was built locally, no serial port was opened, no firmware was
  flashed, no RF transmission occurred, and no SD operation occurred.

### `2026-07-18T16:54:19-04:00`

- A second operator power cycle was observed read-only. Plug-and-Play again
  reports the same `USB-SERIAL CH340 (COM12)` identity present and healthy;
  the controlled listener remains on COM15, and COM16 remains absent. This
  pre-candidate cycle is not release evidence. No serial port was opened.
- Commit `a6f9477` adds the non-secret full 64-hex public key to
  `identity status`, allowing the controlled RF runner to verify the D1L
  fingerprint and the COM15 listener's 12-hex sender prefix from one exact
  identity. Focused identity/profile/source-pin validation passed 27 tests
  with one documented Actions-only skip.
- Every open P1 was reviewed against the Core product contract. Issues #12,
  #17, #18, #19, and #22 are enhancement work for excluded or broader Full
  Feature scope, not reports of a known Core crash, data-loss, or security
  defect. Each now has exactly one `full-feature-deferred` classification and
  an explanatory issue comment; none was closed.
- Pre-freeze review found that the first Core audit draft still consumed old
  reboot/defect schemas and that `storage retained-canary` remained outside
  disabled-SD command admission. R7/R8 remain open until the final audit
  recomputes the hash-bound matrix/API receipts and every SD mutation is
  unreachable. The one full host suite and final Actions candidate remain
  deliberately unstarted.
- No firmware was built locally, no firmware was flashed, no RF transmission
  occurred, and no SD operation occurred.

### `2026-07-18T17:15:21-04:00`

- GitHub state was refreshed again. `origin/main` remains
  `846f728dd3faded85451c6d39ba6a07cb8ca7f44`; PR #197 remains at reviewed
  head `79d3efae9c784d65b70b0d79fae352b3ad5199aa`; excluded draft PR #199 remains
  at `6c854159dfc37525ff4df2d57ca216e4b2303bc2`; no `release/24h-core`
  `d1l-ci` run, `v1.0.0` tag, or release exists.
- Independent pre-freeze review found a real Core reachability blocker:
  retained private-group channel keys could still be selected, decrypted,
  reconciled, and persisted by the always-running Mesh task after a
  non-erasing upgrade. A bounded R6 follow-up now owns a profile-aware
  pre-decrypt Public-only admission boundary and Public-only retained
  reconciliation. Candidate freeze remains prohibited until the integrated
  focused tests prove zero private-channel mutation in Core.
- The same review held the first R7 audit/RF draft for four fail-closed
  evidence repairs: consume the raw exact 5-software/3-cold reboot matrix;
  recompute the exact package-review receipt; recompute the paginated GitHub
  defect snapshot and narrow issue #71 pre-tag exception; and correlate
  ACK/PATH/direct-route evidence to the unique DM token and ACK hash rather
  than accepting stale retained rows.
- The active-soak COM15 adapter now has a green 44-test focused slice for
  before/after raw listener captures, same-run identity, exact successful-send
  counter deltas, zero ACK misses, the D1L sender prefix, a reply-triggering
  `test` token, and the expected transmission floor. It is not integrated
  until the complete bounded R7 suite and the remaining audit/RF repairs pass.
- The operator's power cycle remains pre-candidate only. COM12 has not been
  opened, the full host suite and candidate workflow have not run, and no
  firmware build, flash, RF transmission, or SD operation occurred.

### `2026-07-18T19:13:06-04:00`

- Commit `e3f99a6` closes the retained private-channel runtime gap. Core RX
  filters to the fixed Public key before secret copy/decrypt, non-Public TX
  rejects before reconciliation/key access, and retained reconciliation does
  not mutate private-channel state. Integrated validation passed 17 direct
  boundary/store tests and 53 adjacent MeshCore/source-pin tests.
- Commits `fa2fcf8` and `712c5a3` close three additional Core reachability
  gaps found by the independent freeze review: `packets clear` and
  `nodes clear` now reject before their persisted handlers and are absent from
  Core help; the Core node projection emits no location/GPS fields; and the
  disabled-SD `storage_card`/`storage_data` deep links return the exact
  profile/feature rejection before UI dispatch. Integrated focused validation
  passed 39 tests with one expected Windows fuzz skip.
- Commit `7ba5592` adds strict recomputation of the retained authenticated
  GitHub defect snapshot, including exact run/attempt aliases, raw API page
  hashes and empty sentinels, duplicate-query stability, issue classification
  transitions, critical Core P1 review, and the narrow issue #71 pre-tag
  exception. Its integrated focused suite passed all 24 tests.
- Commit `5260a5f` integrates the exact Actions capture, Core-only package,
  non-erasing COM12 flash, Core smoke/UI runners, strict install review,
  controlled DM/ACK/direct-route receipt, disabled-SD decision, and contiguous
  60-minute active plus 30-minute idle soak audit. The integrated evidence
  slice passed 167 focused tests with one expected Windows symlink-privilege
  skip, and every new Python entrypoint compiled.
- A final producer/audit alignment remains open before freeze:
  `manual_ui_review_d1l.py` must emit the audit-required start/end timestamps
  and explicit no-DM-RF field, refuse overwrite, and receive strict
  recomputation coverage. A bounded follow-up owns that repair; the one full
  host suite and candidate workflow remain unstarted until it lands and the
  read-only freeze review completes.
- Live GitHub refresh still reports `origin/main` at
  `846f728dd3faded85451c6d39ba6a07cb8ca7f44`, reviewed PR #197 at
  `79d3efae9c784d65b70b0d79fae352b3ad5199aa`, excluded draft PR #199 at
  `6c854159dfc37525ff4df2d57ca216e4b2303bc2`, no release-branch `d1l-ci`
  run, and no `v1.0.0` tag or release.
- A transient all-zero Git worktree index/ref/write incident during one
  cherry-pick was detected before commit by byte comparison. Only the exact
  affected paths were restored from verified Git objects, the branch ref was
  rebuilt from the already-pushed commit, object/ref connectivity passed, and
  the bounded commit was then reapplied normally. No user work or source
  change was lost.
- The operator's power cycle remains pre-candidate only. COM12 and COM15 have
  not been opened by this release flow, the full host suite and candidate
  workflow have not run, and no firmware build, flash, RF transmission, or SD
  operation occurred.

### `2026-07-18T19:40:03-04:00`

- The remaining pre-freeze producer/audit alignment is closed. Commits
  `735abe2` and `44bf5ce` make the manual physical UI receipt immutable,
  checksum-bound, exact-run/attempt-bound, explicit about zero DM/Public RF,
  and strictly recomputed by the Core audit. The automated Core UI receipt now
  executes rejected excluded-feature deep links and proves the active Core tab
  remains unchanged.
- Commit `28a8d49` canonicalizes excluded scroll/compose aliases before UI
  handlers, and the combined disabled-SD admission path returns truthful
  unavailable status without probing the RP2040 or media. Commit `c4629ea`
  also enforces a 15-minute GitHub defect-snapshot age with bounded future
  skew. The combined console policy source pins were refreshed in `826d7de`.
- Commits `fc41c10`, `ae9f222`, and `f9e26e0` bind provenance to
  `n30nex/SIGUI`, `d1l-ci`, the canonical workflow path, Core profile, exact
  run and attempt, and then deterministically recompute every subject and
  material against the current clean source and package. The final adversarial
  checksum-preserving subject-digest transplant now fails closed.
- Integrated focused validation after these repairs passed 107 tests with two
  expected platform skips; the final provenance/package/audit slice passed 37
  additional focused tests. Independent runtime and release-flow reviews found
  no remaining concrete pre-freeze Core P0/P1. The single full host suite is
  the next and only remaining R9 action.
- Live GitHub refresh confirms `origin/main` remains
  `846f728dd3faded85451c6d39ba6a07cb8ca7f44`, PR #197 remains open at
  `79d3efae9c784d65b70b0d79fae352b3ad5199aa`, excluded draft PR #199
  remains open at `6c854159dfc37525ff4df2d57ca216e4b2303bc2`, no
  release-branch `d1l-ci` run exists, and no `v1.0.0` tag or release exists.
  Open P1s remain explicitly Full Feature deferred; no known Core
  crash/data-loss/security P1 was introduced.
- The user's power cycle remains pre-candidate availability only. COM12 and
  COM15 have not been opened by this release flow. No firmware was built
  locally, no firmware was flashed, no RF transmission occurred, and no SD
  operation occurred.

### `2026-07-18T19:53:17-04:00`

- The first full host suite completed with 1,495 passed, 7 skipped, and four
  failures. None exposed a firmware runtime defect: one 2026-06 generic
  no-hardcoded-port test predated the authoritative Core port binding, one
  native JSON fixture accidentally extracted the newly inserted command-policy
  block, and two UI contract assertions still expected SD-only attention
  instead of the correct combined retained-backup-or-SD attention state.
- R15 was activated exactly as authorized. The smallest repair changed tests
  only: the port scan now requires an exact closed allowlist of reviewed Core
  release files and literal sets, the native fixture stops at the immediately
  following policy typedef, and the UI assertions require
  `storage_attention`. The runtime source and candidate behavior are
  unchanged.
- Focused validation passed all 107 affected port, flash, smoke, reboot,
  RF/DM, soak, native JSON, and storage-attention tests. One new full suite is
  now required by R15 before candidate freeze. No firmware was built locally,
  no serial port was opened, no flash or RF transmission occurred, and no SD
  operation occurred.

### `2026-07-18T20:20:36-04:00`

- Candidate `8ff4d765272791796f8f08d0a1fa044024f652e3` passed the complete
  host suite with 1,499 passed and 7 skipped. Workflow-dispatch Actions run
  `29666410430` then passed host, MeshCore conformance, ESP-IDF firmware,
  package, provenance, and SBOM jobs with `include_sd_bridge=false`.
- The strict capture downloaded exactly five archives. Each archive SHA-256
  matched its GitHub API digest, all 343 extracted files received immutable
  per-file and aggregate hashes, and all embedded firmware, build-input, and
  package checksum trees passed.
- Before COM12 was opened, the flash runner revalidated the receipt and found
  a release-script defect: ZIP members were ordered by canonical POSIX text,
  while extracted paths used host-native `Path` ordering. On Windows,
  mixed-case names such as `SHA256SUMS.txt` therefore compared in a different
  order despite identical paths, sizes, hashes, counts, and sets.
- The fail-closed gate worked: no flash occurred. Candidate `8ff4d765…` and
  run `29666410430` are superseded and will not be reused as release evidence.
  R15 is activated again for the smallest repair: sort extracted inventory by
  repository-relative POSIX path and cover the exact mixed-case ZIP/tree
  regression. A new focused suite, full suite, and Actions candidate are
  required. PR #200 remains the single integration PR; its automatic duplicate
  run `29666726329` was canceled.
- No serial port was opened, no firmware was flashed, no RF transmission
  occurred, and no SD operation occurred.

### `2026-07-18T21:07:52-04:00`

- R15's mixed-case inventory repair produced candidate
  `a9f14afe58aaa307a40e90378c64db7a464e59bc`. Its complete host suite passed
  1,500 tests with seven skips. Exact workflow-dispatch run `29667030146`,
  attempt 1, succeeded with `include_sd_bridge=false`; its five downloaded
  archives and every embedded checksum tree matched the GitHub API and package
  manifests.
- The exact package was flashed non-erasing to COM12. The first smoke receipt
  found two persisted panic rows that predated the candidate's first POWERON
  boot. Their sequence chronology and the candidate's later POWERON/SW rows
  were preserved in a one-time baseline-clear receipt before the ring was
  cleared. A fresh unique Core smoke then passed all 13 checks, including an
  empty crashlog, exact candidate/profile/IDF identity, disabled-SD/NVS truth,
  and software-reboot persistence.
- The exact 20-round UI receipt completed all 120 navigation/data-refresh
  events with zero tab, identity, telemetry, health, or crash failure. It
  failed five raw diagnostic checks: Home, an empty DM thread, and compact
  Core Settings were correctly present but had no positive vertical overflow;
  DM and DM-long geometry sheets were rejected because the diagnostic used a
  fabricated key through strict production full-key admission.
- The candidate failed closed before reboot, RF, or soak qualification. No
  Public RF, network transmission, SD formatting, or SD/RP2040 action
  occurred. Candidate `a9f14afe…`, run `29667030146`, and their physical
  receipts are superseded and cannot qualify the repaired SHA.
- R15 is activated for the smallest diagnostic repair. Firmware now emits
  `movement_required` from the raw LVGL bounds; the Core producer and final
  audit independently recompute overflow and movement, so positive overflow
  still requires real motion while a fit-only surface is not mislabeled as
  corrupt. The generic Full Feature runner remains unchanged.
- Production contact-to-DM admission still resolves and validates the exact
  full key before opening compose. Only the static diagnostic path presents a
  synthetic geometry sheet, with an explicit `tx_suppressed=true`,
  `send_enabled=false`, and a pre-send hard return before either channel or DM
  transmission call. The producer and final audit recompute the raw target,
  tab, mode, geometry, RF/format flags, and suppression state.
- Focused R15 validation currently passes 88 producer, audit, UI, modal,
  identity, eligibility, hardware-script, and Core-profile tests. Independent
  review, affected source-pin checks, the new full host suite, and a fresh
  Actions candidate remain required before COM12 can be flashed again.

### `2026-07-18T21:50:26-04:00`

- R15 is complete. Firmware emits explicit scroll applicability from raw LVGL
  bounds, while both the Core producer and final audit independently require
  real motion whenever positive overflow exists. Fit-only Home, empty DM, and
  compact Settings surfaces are no longer mislabeled as corrupt.
- Production contact-to-DM admission remains bound to an exact full key.
  Only the static diagnostic path may present its synthetic geometry sheet;
  that path sets `tx_suppressed=true`, keeps send disabled, and returns before
  either Public-channel or direct-message transmission call.
- The standalone scroll runner now has an explicit `core_1_0` mode with the
  exact ordered six-surface plan, clean 40-hex candidate and Actions identity,
  disabled-SD truth, COM12 admission before serial open, and read-only
  identity preflight before any crashlog/UI mutation. Generic Full Feature
  behavior and output remain unchanged by default.
- Final focused validation passed 100 tests; the integrated manual/audit/UI
  evidence slice passed 77 tests. Canonical source hashes were recomputed and
  `git diff --check` passed. Three independent reviews found no remaining
  concrete Core P0/P1 in the final R15 delta.
- The complete repaired-tree host suite passed 1,507 tests with seven expected
  skips in 446.45 seconds. A prior diagnostic run exposed only one stale
  strict-receipt fixture; after that fixture was repaired, the clean required
  suite passed without failures.
- GitHub was refreshed after the suite. `origin/main` remains
  `846f728dd3faded85451c6d39ba6a07cb8ca7f44`; PR #197 remains open at
  `79d3efae9c784d65b70b0d79fae352b3ad5199aa`; excluded draft PR #199 remains
  open at `6c854159dfc37525ff4df2d57ca216e4b2303bc2`; PR #200 remains the only
  Core integration PR; no `v1.0.0` tag or release exists; and no Actions run
  is active or queued.
- No local firmware build occurred. The superseded candidate remains on
  COM12; no new serial, RF, SD, or RP2040 operation occurred during R15.

### `2026-07-23T18:18:10-04:00`

- The authoritative 24-hour elapsed cap has expired without a releasable
  candidate. `core_release_ready=false` and
  `full_feature_release_ready=false`; GitHub has no `v1.0.0` tag or release.
  Work continues under R15, but the missed deadline and every remaining
  physical gate stay explicit no-go conditions.
- Candidate `f0fef39eb37f5795ebd683338c11a37ff2fc8399` passed the complete
  host suite and exact workflow-dispatch `d1l-ci` run `29669268575`, attempt
  1, with `include_sd_bridge=false`. The five downloaded Actions archives,
  their extracted inventories, and embedded checksum trees passed. The
  application image SHA-256 is
  `c5c9436f65ccf9d9d11e4900769fd78c92f2dd66835a26715b51e60a1b8ec032`;
  the full 8 MiB image SHA-256 is
  `0128aa5e7603483d69dc2215ee39c724302d51814b66c814717fd54511a73696`.
- The exact package received a non-erasing COM12 bootstrap flash. Its flash
  receipt is intentionally `closure_eligible=false`. Fresh exact-candidate
  Core smoke passed, and the strict Core UI probe passed 20 rounds / 120
  navigation and refresh events with the five supported tabs and excluded
  destinations rejected.
- The immutable retained-state seed receipt then failed closed. The initial
  raw capture proved a valid Public page response was rejected because its
  command label omitted the requested offset, only eight of sixteen contacts
  were projected, and the Public, DM, and contact stores were at their
  bounded capacities. Candidate-local retained and settings mutations were
  skipped after the failed initial capture, so this attempt caused no
  eviction, deletion, overwrite, Public RF, DM RF, SD, or RP2040 operation.
  All `f0fef39...` physical receipts are now superseded for closure.
- The exact device also reported
  `protocol_tx_ready=false`, legacy retained value `1767225743`, and
  `legacy_protocol_lower_bound_unconfirmed`. No wall time was inferred and
  no migration was attempted. A separate fail-closed exact-device migration
  evidence runner is under review and cannot execute before a replacement
  exact Actions candidate is present on uniquely identified COM12.
- Independent review rejected the first retained repair commit `0332d3d`
  for partial-mutation, retry-truth, global-flush, output-reservation, and
  snapshot-coherence gaps. It also rejected simulator commit `e8f27d1`
  because one Full-Feature alias remained accepted, coordinate-bearing node
  detail could trap Core navigation at Map, and excluded Route/Trace and QR
  actions remained visible. Both branches are being repaired; neither commit
  is integrated or candidate evidence.
- The controlled peer now lives on the Pi 5 rather than local COM15. A
  fail-closed SSH/raw-status/UDS adapter and matching active-soak evidence
  binding are under host-only test. Live use remains blocked until valid
  noninteractive Pi authentication is available; the forbidden COM11 peer is
  excluded and untouched.
- A read-only phase-boundary enumeration currently finds no COM12 and no
  COM16. No port was remapped or substituted. The next firmware-changing
  action remains a new GitHub-Actions-only candidate; no local firmware build
  is permitted.
- Live GitHub refresh shows `origin/main` still at
  `846f728dd3faded85451c6d39ba6a07cb8ca7f44`, reviewed PR #197 open at
  `79d3efae9c784d65b70b0d79fae352b3ad5199aa`, excluded draft PR #199 open
  at `6c854159dfc37525ff4df2d57ca216e4b2303bc2`, and draft PR #200 as the
  sole Core integration PR at `f0fef39eb37f5795ebd683338c11a37ff2fc8399`.
- C: free space is monitored before long phases with a 4 GiB safety floor;
  it was 4.89 GiB at this update. Test temporary files are redirected to F:,
  and no background or visible terminal processes are used.

### `2026-07-24T05:46:00-04:00`

- The operator moved the release-closing D1L from the Windows host to the
  Raspberry Pi 5 development host `neopi5`. Future physical Core evidence must
  use the unprivileged, key-only account `siguidev` and the stable device link
  `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`.
- Read-only enumeration identifies that link as USB VID:PID `1A86:7523`. It
  currently resolves to `/dev/ttyUSB2`, but that raw kernel-assigned name is
  observational only and must never be substituted in a flash, serial, audit,
  or evidence command. `COM12` remains a valid Windows alternative only if the
  D1L is intentionally moved back.
- Historical COM12 receipts remain immutable evidence for the exact candidates
  they name. They are superseded for release closure and are not rewritten as
  Pi evidence. `COM8`, `COM11`, and `COM29` remain forbidden; `COM16` remains
  reserved for separately authorized SD/RP2040 work and is never the Core D1L
  target.
- The Pi move, key-only login, serial permissions, and read-only device
  handshake do not make the firmware release-ready. A fresh exact-SHA Actions
  candidate, verified package, non-erasing flash, Core UI/manual review,
  software/cold reboot and retained-state proof, protocol-time migration,
  controlled RF/DM, 60-minute active plus 30-minute idle soak, install review,
  and final Core audit all remain fail-closed.
- Controlled-peer RF and active-soak evidence remain blocked until `siguidev`
  receives and proves narrowly scoped access to only the required peer status
  and control resources. Broad service-group membership is not recorded as a
  completed prerequisite.
- This routing correction changed documentation only. It did not build or
  flash firmware, transmit RF, touch SD, qualify a candidate, or authorize a
  tag/release.
