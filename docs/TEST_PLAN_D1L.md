# D1L Test Plan

## Exact Full Feature candidate plan (2026-07-25)

The active candidate is `release_profile=full_feature` with
`sd_history_mode=conditional`. Feature implementation is complete. Validation
is intentionally limited to the release-critical gates below; historical
edge-case campaigns are not being repeated unless the exact candidate fails a
required gate.

Required sequence:

1. Run the repository host suite, completion-ledger validation, generated-pack
   validation and `git diff --check`.
2. Push one immutable commit and require exact-SHA `d1l-ci` success.
3. Download the exact package; verify all checksums, provenance, SBOM, Actions
   identity, release profile, signed update signer and security sequence.
4. On Pi 5 host `neopi5`, prove the target symlink is
   `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`, prove
   `ID_VENDOR_ID=1a86` and `ID_MODEL_ID=7523`, then flash that exact package.
   Never enumerate or probe another Pi serial device.
5. Run automated version/health/board/settings/radio/identity/storage/BLE/
   Observer/Admin/update/terminal probes, required UI navigation/scroll
   probes, message/store checks, controlled reboot and persistence checks.
6. Run conditional SD/RP2040 checks without formatting. If the required media
   path is absent or unsafe, report the exact blocked gate; do not erase it.
7. Use only a narrowly authorized controlled peer for DM/RF/admin acceptance.
   Never automate a default Public transmission.
8. Ask for one final consolidated physical confirmation covering display,
   touch, keyboard, brightness/timeout wake behavior and the five dock roots.

Pass requires receipts bound to the exact firmware commit and Actions run.
Predecessor artifacts, simulator images, dry runs, a reachable serial port or
a green source test alone do not pass.

The current device has moved permanently for this development cycle. Raw
`/dev/ttyUSB*` names and stale Windows COM assignments are not valid target
identity.

The remaining content is retained historical test rationale and prior exact
candidate evidence.

Last strict-verified exact-main software/artifact checkpoint: PR #182 at `de0bb75bd91146f0dc9896540d12c71889d7766b` / Actions `29548300732` passes 1,263 host and 33 checksum-contract tests plus 1,008 wire vectors, 931 oracle checks, existing wire/advert fuzzing, and 100,000 native plus 100,000 Clang 18 semantic-packet cases with zero findings. Five downloaded artifacts / 46 entries verify across 341 files with exact-source provenance and SPDX 2.3. The canonical strict receipt SHA-256 is `8da06d90df77a439e37892560272f902243776107365a1676fdd5a49824b74d9`; canonical PR Actions `29547584817` and exact main share the reviewed tree. PRs #177-#182 cover replay/hash authority, bounded USB parsing, ACK/lifetime behavior, configured-channel admission, and semantic-packet parsing/fuzzing. A non-erasing exact-main COM12 flash passed, but retained-DM compatibility/migration must be fixed before reboot persistence can pass. Full WP-05, WP-06, official-peer RF, exact-candidate physical acceptance, and release gates remain open.

## Current Core hardware target (2026-07-24)

The release-closing D1L is now attached to Raspberry Pi 5 host `neopi5`.
Hardware tests run from the unprivileged, key-only account `siguidev` and use
only `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0` after proving USB
VID:PID `1A86:7523`, symlink resolution, and read/write access. The current
`/dev/ttyUSB2` resolution is observational only and is never a release target.
The old `COM12` assignment is historical and is not a current alternative.
The legacy COM restrictions below remain preserved only for interpreting old
receipts; current work never routes through them.

On `neopi5`, begin every hardware evidence session with:

```bash
export D1L_PORT='/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
test -L "$D1L_PORT" && test -r "$D1L_PORT" && test -w "$D1L_PORT"
D1L_DEVICE_PROPERTIES="$(udevadm info --query=property --name="$D1L_PORT")"
grep -qx 'ID_VENDOR_ID=1a86' <<<"$D1L_DEVICE_PROPERTIES"
grep -qx 'ID_MODEL_ID=7523' <<<"$D1L_DEVICE_PROPERTIES"
```

All literal COM12 receipts and checkpoint statements retained below are
historical evidence for the exact candidates they name, or Windows-alternative
examples; they are not current Pi evidence and must not be relabeled. The Pi
move, login, discovery, and serial permissions close no gate. Exact-SHA
Actions/package identity, non-erasing flash, UI/manual review, software and
cold reboots, retained state, protocol-time migration, controlled RF/DM,
60-minute active plus 30-minute idle soak, install review, and final Core audit
remain fail-closed. Do not start RF or active soak until `siguidev` has
narrowly scoped, verified access to the exact peer status/control resources.

## Host Tests

Run:

```powershell
python -m pytest tests
python .\tools\ui_simulator.py --out artifacts\ui-sim
python .\tools\ui_simulator.py --scenario large-mesh --out artifacts\ui-sim-large
python .\tools\ui_simulator.py --scenario storage-states --out artifacts\ui-sim-storage
python .\tools\ui_simulator.py --scenario map-ready --view map --view map_options --view map_location --view map_cache --out artifacts\ui-sim-map-ready
python .\tools\ui_simulator.py --scenario map-downloading --view map --out artifacts\ui-sim-map-downloading
python .\scripts\smoke_d1l.py --dry-run
python .\scripts\ui_corruption_probe_d1l.py --dry-run --rounds 20
python .\scripts\ui_capture_d1l.py --dry-run
python .\scripts\scroll_probe_d1l.py --dry-run --screens home,public_messages,dm_thread,nodes,packets,settings,storage,storage_card,storage_data,wifi,map,map_options,map_location,map_cache
python .\scripts\autonomous_hardware_validate_d1l.py --github-run-id <run-id> --github-run-dir artifacts\github\<run-id>-current --commit <sha> --dry-run
python .\scripts\probe_d1l_dm.py --dry-run --port COM12 --peer-port <explicit-allowed-peer-port> --peer-status <controlled-peer-status.json>
python .\scripts\rf_full_acceptance_d1l.py --dry-run --port COM12
python .\scripts\sd_reboot_remount_acceptance_d1l.py --dry-run --token dryrun
python .\scripts\soak_d1l.py --dry-run --duration-sec 60 --sample-interval-sec 15 --active-dm-fingerprint 0123456789ABCDEF --active-dm-text test --active-interval-sec 30 --require-rx-delta --min-tx-delta 1 --clear-crashlog-before-start
python .\scripts\soak_d1l.py --dry-run --duration-sec 60 --sample-interval-sec 15 --sample-storage --sd-file-canary --allow-sd-unavailable
python .\scripts\release_gate_audit_d1l.py --out artifacts\release-gate\d1l-release-gate-audit-local.json
```

Coverage:

- Canada/USA radio defaults.
- D1L pin map and TCXO `NONE`.
- RP2040 bridge pin contract.
- No hardcoded executable COM ports.
- Smoke JSONL parser.
- Targeted DM-only hardware probe can validate D1L-to-controlled-peer DM paths without Public-channel RF when the operator explicitly assigns two allowed, distinct ports.
- Flash/monitor scripts require an explicit port.
- Backup command builder.
- Checksum verifier.
- MeshCore 3-byte companion transport codec.
- MeshCore wire-envelope conformance contract: the separate Ubuntu 24.04
  `meshcore-conformance` job must recursively check out the pinned MeshCore
  submodule, use `clang-18`/`clang++-18` with
  ASan+UBSan/libFuzzer, run 100,000 deterministic local-parser inputs with
  seed `13746277` (`0xD1C065`), upload
  `artifacts/meshcore-conformance/meshcore_conformance_<full-commit>.json`, and
  gate `firmware-build`. The result must report
  `coverage_level="wire_envelope_only"` and `closure_ready=false`. The job runs
  the exact-commit signed-advert runtime before conformance, validates that
  sanitized receipt, and binds its canonical SHA-256 plus five distinct signed
  timestamp outcomes, five upstream/D1L packet-hash outcomes, and the exact real
  `SimpleMeshTables` lookup/duplicate receipt into the conformance receipt.
  Actions, release packaging, reproducibility comparison, and release audit
  must recompute that canonical digest from the supplied exact-commit
  signed-runtime receipt and reject any substituted binding. The declared WP-05
  host matrix is currently 11 production suites / 75 scenarios / 38 translation
  units / 73 source pins, plus 1 pinned-upstream companion suite / 10 cases.
  The separate USB parser target executes the production USB command-admission module for
  100,000 deterministic inputs. The separate pinned-Clang gate must also run
  `scripts/usb_command_parser_fuzz_d1l.py` for exactly 100,000 libFuzzer inputs,
  upload `usb_command_parser_fuzz_<full-commit>.json`, and prove that both the
  normal and factory-reset recovery consoles pass the real received byte count
  before any command dispatch. Embedded NUL, C0/DEL, hidden suffix, empty,
  truncated-argument, token-suffix, and over-255-byte cases must fail before
  side effects and wipe the complete command buffer. This is host/parser proof,
  not hardware, RF, WP-05, or release closure.
  The job must independently run
  `scripts/meshcore_semantic_packet_fuzz_d1l.py` with pinned `clang-18`, seed
  `13746277`, and exactly 100,000 libFuzzer inputs, then upload
  `meshcore_semantic_packet_fuzz_<full-commit>.json`. Its 100,000-case native
  contract and 14 hash-bound seeds must cover every locally dispatched packet
  family, malformed/truncated/future-version/unsupported inputs, family length
  boundaries, invalid multipart subtype, maximum advert data, and oversize
  advert rejection. Every reject must leave a canonical zero output and must not
  reach a semantic handler. This implements only the structural
  `semantic_packet_parser` target; decrypt/auth, state/replay, RF, physical,
  WP-05, and release closure remain open.
  Native cases also require a 160-entry cyclic cache, correct all-zero occupancy,
  deterministic FIFO eviction, multipart-ACK descriptor normalization, and
  terminal-only admission. Production source contracts must prove authentication
  and semantic authority before probing. Configured-channel RX must scan all
  eight bounded hash matches, accept exactly one MAC-authenticated secret, and
  reject unknown, unauthenticated, or multiply authenticated dispatch without a
  retained side effect. Configured-channel TX uses the exact selected channel
  secret even when its routing hash collides; its exact packet hash enters the
  boot-local seen cache only after successful terminal TX so an RF echo cannot
  create a self-message. DM candidates require current canonical contact
  authority and reject the full local public key before shared-secret work.
  The remaining production contracts require one visible channel/DM row; bounded DM
  re-ACK; exact-owner ACK persistence/reconciliation across simple, multipart,
  and PATH encodings, with the coordinator armed from the actual
  `AWAITING_ACK` revision, the retained RF receipt bound to that same session,
  base revision, and ACK hash, and deadline/route/hash effects authorized once
  only after durable publication; one-shot authenticated PATH effects with ACK-only retry;
  pending-first TRACE correlation with partial-retention retry; and terminal
  advert receipts. Executable lifetime checks require an inclusive 30-minute
  contact/route boundary, unsigned monotonic-clock wrap behavior, explicit
  node-reachability invalidation on reload while historical `last_heard_ms`
  remains intact, exact 160-entry packet FIFO rollover, and terminal
  `UINT32_MAX` advert timestamps.
  These bounded checks do not prove persisted generic cache, retained-state
  fault/power-loss recovery, hardware, real-peer RF, complete-surface, or issue
  #65 closure. See
  [MeshCore Conformance Boundary](MESHCORE_CONFORMANCE.md).
- Full MeshCore conformance release contract: the passing wire-envelope package
  gate is only a prerequisite. A separate P0
  `meshcore_full_conformance_complete` gate remains failed until issue #65 has
  a distinct current-commit closure artifact covering the declared semantic,
  cryptographic, retained-state, duplicate/replay, and peer-interoperability
  surface. Structural evidence must never make `ready_for_public_release=true`.
- Declared WP-05 semantic-matrix contract: run
  `python -m pytest --cache-clear -q tests/test_meshcore_conformance_contract.py tests/test_meshcore_oracle_contract.py`.
  The current PR #176 checkpoint requires 28 passing focused tests, exact enumeration
  of 16 semantic and 8 fuzz-target classes, 6 ASan/UBSan production suites / 34
  scenarios, and 46 compiler-derived repository-local dependencies from 20
  one-translation-unit `-MM` scans. Missing, stale, substituted, or dry-run-only
  evidence must fail closed. The receipt must preserve the truthful 7/8/1
  implemented/partial/missing semantic counts and 2/1/5 fuzz-target counts;
  these partial counts prohibit WP-05 or release closure.
- Packaged conformance provenance contract: its manifest source must be clean
  after the allowlisted tracked BSP patch check, its workflow SHA and run ID
  must match the selected audit run, and its commit-named JSON, size, SHA-256,
  generated/expiry timestamps, and non-closure boundary must validate without
  path escape or timestamp overflow.
- Production/oracle Ed25519 arithmetic contract: production, the main host
  oracle, and the signed-advert runtime must select the reviewed
  `overlays/meshcore_ed25519_defined/` replacements for `fe.c`, `ge.c`, and
  `sc.c` while retaining the pinned MeshCore gitlink and all other upstream
  sources. Exact receipts must report all 215 reviewed transformations, 256
  byte-identical baseline/overlay differential cases, RFC 8032 coverage,
  `full_ubsan_clean=true`, an empty sanitizer-exception list, zero sanitizer or
  memory errors, and raw/canonical signed-runtime binding. Any
  `-fno-sanitize=` escape, missing package/SBOM/provenance binding, or source
  selection drift fails closed.
- NVS settings contract and default-off Wi-Fi/BLE/observer policy.
- Nodes role-summary contract: count only the exact bounded rows returned to the Nodes renderer, group exact `chat` and `companion` as Chat, count exact `repeater`, `room`, and `sensor` separately, and fail closed for every missing, differently cased, spaced, aliased, or future role as Unknown. The count sum must equal the rendered query count up to `D1L_NODE_STORE_CAPACITY` (64), null input must reset counts, and neither path, signal, nor display-name inference may affect a role total. Host coverage must include default, empty, mixed-role/unknown, and capacity-bounded large-mesh scenarios and prove Nodes navigation adds no RF, formatting, or destructive action. PR #165's strict software/artifact proof does not substitute for exact-candidate physical Nodes/Map acceptance or `network_map_nodes_<sha>_<d1l>.json`.
- Node Detail contract: the retained fingerprint selects a bounded projection with identity, role, last-heard, route, signal, key/capability, and advert-location truth without inferring unsupported state. Message is enabled only when the retained capability permits it; Admin remains a passive locked state unless the authorized protocol/runtime path exists. Open-from-Map and Close must preserve the retained Map view. PR #172 exact-head coverage is 74 focused tests; its documented combined Node Detail plus conformance set is 106. PR #175 separately banks the bounded signed-location Map-pin contract below. These software/artifact proofs do not substitute for remaining capability/admin actions, Map-provider qualification, exact-candidate physical UI/RF acceptance, `network_map_nodes_<sha>_<d1l>.json`, or WP-16 closure.
- Phase 5/Phase D connectivity contract: `wifi status`, `wifi scan`, `wifi save <ssid> [password]`, `wifi connect`, `wifi clear`, `wifi on|off`, `ble status`, and `ble on|off` must be machine-readable. `wifi save` must persist SSID/password intent without printing the password, `wifi scan` must return bounded network records only when Wi-Fi is enabled and compiled in, `wifi connect` must never echo the password, `wifi clear` must remove the saved profile and disable Wi-Fi intent, and BLE remains gated until a measured runtime is enabled.
- Phase 7 diagnostics contract: `crashlog` must return a bounded persisted reset ring, `crashlog clear` must clear it, and `health` must include heap/PSRAM largest blocks, task stack watermarks, LVGL usage, reset reason, and board/UI readiness. `retained_task_stack_free_bytes` is the ESP-IDF byte high-water mark for the combined Public/DM/packet/route persistence worker; the release soak fails below 4096 bytes after late-SD reconciliation and forced flush.
- Boot/recovery diagnostics contract: every boot generates one nonzero in-RAM `boot_nonce`; all `health` responses during that boot must retain it and a real reboot must change it. A successful controlled reboot response, every reboot acceptance runner, and the release audit must also require `route_flush="ESP_OK"`; `ok=true` plus `rebooting=true` is not sufficient route-durability evidence. `health` and the boot event must expose `nvs_ready` plus `nvs_error`. `ESP_ERR_NVS_NO_FREE_PAGES`, an unsupported NVS version, or another NVS-init failure must never erase the NVS partition or reboot-loop to make validation pass; firmware preserves the partition, boots in a degraded fail-closed state, and reports the error for recovery.
- Retained-NVS telemetry contract: `storage status.retained_nvs.telemetry` must report `scope="boot_runtime_api_calls"`, `physical_flash_cycles_measured=false`, a fail-closed `capacity.valid`/`capacity.error` pair, `used_entries`, `free_entries`, `available_entries`, `total_entries`, and `namespace_count`, plus global and per-store write attempts, successful commits, attempted/committed bytes, failures, erase attempts/commits/failures, and last error. Native tests must cover a successful Public commit, a failed DM commit, a scoped erase, and capacity snapshot behavior; source contracts must preserve saturating counters. Exact-candidate write-amplification acceptance must capture the same boot nonce before and after a named packet/message burst and reconcile these API counters with retained-scheduler dirty/coalesced/commit counts; API counters alone are not physical flash-wear or endurance proof.
- Factory-reset contract: run `python -m pytest -q tests/test_factory_reset_contract.py tests/test_factory_reset_native.py tests/test_retained_blob_store_backend_state_native.py tests/test_settings_protocol_migration_contract.py tests/test_mesh_runtime_queue_fairness_contract.py tests/test_meshcore_runtime_guard_native.py tests/test_meshcore_conformance_contract.py`; PR #174 requires 38 passes. `factory-reset-confirm` must quiesce storage, the retained worker, RP2040 bridge, and connectivity, durably journal intent, and reboot without deleting live state. Boot must resume before producers; corrupt, future, or unreadable journals must enter producer-silent USB recovery, whose repair command requires a 60-second random token and the exact `ERASE_ONBOARD_USER_STATE` phrase. Clearing is limited to the 16 documented owned NVS domains with per-key commits and restart-from-domain-zero retry; namespace/partition erase is forbidden. Four SD lineage fences must arm before clearing. The coordinator must never probe, mount, delete from, write, or format SD; later reconciliation may delete only the documented Public/DM/route/packet owned files after an exact generation check and must preserve unrelated files. Software proof does not close physical cuts across every clear boundary, repeated-power-loss recovery, absent/insert/swap/remove/reinsert/reboot card-lineage behavior, the bridge inside-one-delete replacement window, flash endurance, WP-11, or release.
- Phase 7 soak harness contract: `scripts/soak_d1l.py` must have a dry-run path, must sample `health`, `mesh status`, `signal`, `messages unread`, `packets`, and `crashlog`, and must summarize uptime monotonicity, readiness, packet deltas, heap/PSRAM deltas, current/UI/retained-worker stack floors, LVGL peak usage, command retries, and crash-like reset entries. With `--sample-storage`, it must also sample `storage status` and summarize SD state/backend stability plus store backend stability. With `--sd-file-canary`, it must also run `storage filecanary`; pre-RP2040-flash `ESP_ERR_NOT_SUPPORTED` preflight refusals are accepted only when `--allow-sd-unavailable` is explicitly set. File-canary gets a bounded 120-second host window; any command timeout or intervening boot marker is terminal for the soak and cannot be retried or followed by another console command. The 12-hour gate independently checks every raw sample, exact allowlisted telemetry commands, a stable nonzero `boot_nonce`, uptime/host-elapsed continuity, the retained-worker floor, and absence of any `mesh send` command. Automated active soak accepts only a validated direct-message fingerprint/text pair (or the dedicated `#test` channel after channel selection exists); every soak must report `public_rf_tx=false` and `formats_sd=false`.
- WP-01 storage-active source contract: run `python scripts/storage_active_soak_d1l.py --port COM12 --expected-firmware-commit <40-hex-sha>` only against the checksum-verified exact pair. The default source alternates four 1800-second storage-aware segments with three strict retained-canary reboot/remount cohorts, sends no Public or DM RF, never formats SD, accepts no transport retries/timeouts, and records explicit `public_rf_tx=false`, `dm_rf_tx=false`, and `formats_sd=false` on both the source and every retained canary. `wp01_evidence_produce_d1l.py --kind storage_active_soak` independently rebuilds the canonical two-hour artifact from raw samples and reboot transcripts; summaries alone cannot claim dirty events or release closure.
- WP-01 diagnostic-isolation contract: run asynchronous `storage diag raw` only in an isolated maintenance boot and do not begin retained canaries after a diagnostic merely because an additional 750 ms ordinary timeout elapsed. After diagnostic capture, reset and reflash the checksum-verified exact Actions artifacts, then require a clean `READY_SD` preflight with zero retained read/write/rename failure counters before file, retained, reboot, inserted-card, remove/reinsert, or soak evidence begins. Any overlap with the one-second retained worker or any pre-canary counter contamination invalidates that evidence window.
- Phase 8 release package contract: `scripts/package_release_d1l.py` must emit a normal flash set, app update image, full 8MB image, manifest, SHA256SUMS, README, and explicit-port flash helpers.
- CI scope contract: `.github/workflows/d1l-ci.yml` must keep ESP32/UI work on the fast default path. The default Actions run builds ESP32 firmware and the release package without rebuilding RP2040 artifacts or running SD/RP2040 host dry-runs; `workflow_dispatch` with `include_sd_bridge=true` or changes under SD/RP2040 paths must opt into the bridge UF2, SD smoke UF2, and SD dry-run checks. Packaging must still wait for the complete host job, whose final step emits the exact-commit/run `d1l_host_checks_success` marker only after every prior host step passes.
- Supported-SDK policy contract: the ESP32 firmware job must use the reviewed `espressif/idf:v5.5.4` OCI index digest recorded in `.github/d1l-build-inputs.json`, and the committed component lock must record IDF 5.5.4; tag-only, `latest`, moving `release-vX.Y`, EOL, missing, unapproved, malformed, and stale selections must fail the P0 `supported_sdk_baseline` audit. Exact Actions evidence must also preserve the recorded build-input bytes and digest; these host checks still do not replace exact-device `version.idf` and behavioral qualification.
- SDK generated-state contract: ESP-IDF Component Manager must generate `dependencies.lock` in the version-pinned Actions environment. The first migration run must archive the exact generated lock and diff for review; after that output is committed without hand-editing its generated hash, the qualifying repeat Actions build must leave the lock unchanged and must surface unexpected generated configuration drift for review.
- Release gate audit contract: `scripts/release_gate_audit_d1l.py` must fail closed when P0 production evidence is missing, must not require hardware or ports in CI, and must require an explicit numeric GitHub run ID whenever it consumes a downloaded run/package. That selected run must contain the matching final `d1l_host_checks_success` marker. Every hardware artifact must contain at least one canonical 7-to-40-hex commit claim, and all recognized commit claims must agree with the exact candidate; a matching filename or one matching field cannot mask missing or stale metadata. The audit must reject obsolete SD preflight evidence that recommends any device-format action, require SD evidence to report `formats_sd=false`, and report `ready_for_public_release=false` until current-commit smoke, compose-keyboard capture, SD matrix, 12-hour soak, manual physical UI/photos, and full RF/DM evidence are present. Exact live-main Actions `29534773905` on `9686c3d7fa52bb1b3c236b930a8c7383527125ac` truthfully reports **33 P0 failures / 35 overall** with audit SHA-256 `25065172ab59e2b6c1e50114009f9b1ea5ac466c34b37d03e57ecbf3e2a29c73`. Remaining protocol/runtime/durability, RF, physical, WP-24, and Full Release gates remain fail-closed.
- Inbound-DM ACK durability contract: schema v5 retains the full 32-byte message identity digest plus per-row reservation count, route kind, lifecycle state, and last error. Valid v1-v4 rows remain visible as `legacy_unverified` but never hydrate collision-safe ACK dedupe state. On a newly accepted v5 row, every durable backend required by the configured store must reserve before RF; partial persistence returns a truthful error and emits no RF. The first attempt may use the selected direct path and the sole retry may fall back to flood, optionally with PATH; no identity may exceed two physical ACK dispatches across callbacks, reboot, late-SD reconciliation, or row resequencing. Callback correlation uses the full digest as primary identity and treats a row sequence only as a stale-safe hint. Boot normalization emits zero RF. TxDone and timeout must persist truthful `sent`, `retryable`, or `terminal` state and error. Host mutations must cover split SD/NVS failures, same-digest/different-sequence reconciliation, higher-epoch resequencing, stale callbacks, pending route-kind rebind, reboot, malformed v5, and schema migration. PR #98 banks this software boundary in `docs/completion/evidence/wp04/ack_durability_76b07f28918b338bf896d5a1a8a0207b5a112677.json` (SHA-256 `351433586b62cdb4761d41a0dee146c8af4f0edd3c3b6e6e897228ea22e00f3c`); exact peer/RF and controlled power-loss evidence remain separate physical gates.
- Learned-route selection contract: a direct path is eligible only when its canonical encoding is known, it was authenticated during the current boot, and its unsigned age is no greater than 1,800,000 ms; a known zero-hop path is valid under the pinned MeshCore rule. Missing, preboot, stale, and malformed path state must produce a complete flood plan. The selected path is copied into one immutable result before transmission, ACK planning uses the same selector, and route telemetry records that actual result instead of rereading mutable contact state. PR #99 banks this software boundary in `docs/completion/evidence/wp04/route_selection_d5acf0a80af4ff533f48105ea84844bd0d9af6c3.json` (SHA-256 `e38a442100cf27f433777eaf6d293b40c44e08a555e406fd7b5d0bb79f9459ac`). PR #101 banked the predecessor explicit-loop TRACE request/correlation/retention proof. The current candidate replaces that operator path with fingerprint-only runtime derivation from one exact canonical contact and current-boot proven one-byte path; controlled-peer RF, multi-byte routing, reboot/power-loss, and physical qualification remain open.
- Identity/contact truth-primitives contract: persisted identity state must classify as absent, consistent, or inconsistent and fail closed on partial/zero material, an unclamped scalar, a uniform nonce prefix, or any mismatch between the stored private material and the entire derived 32-byte public key. Verified-contact storage treats the exact 64-hex public key as authoritative and the 16-hex fingerprint as a matching hint only; ambiguous identity, capacity exhaustion, or NVS failure must not mutate or evict contacts, and verified updates preserve user preferences and learned paths. PR #103 banks this component boundary in `docs/completion/evidence/wp04/identity_contact_truth_primitives_afaa02a48e6a76d99630da81bac80d16e209e212.json` (SHA-256 `67295cd1c4ffbb8ffba22bd78049e7f1bb8bc0a0edb0c84d13d803eeaa24ae7e`). Production signed-advert RX/contact binding, signing lifecycle, reload/UI/sendability truth, RF, and physical qualification remain open.
- Authenticated-admin host-fixture contract: the pinned fixture uses full 32-byte identities, exact request-tag matching, direct and flood-with-PATH-return routing, and a 60-byte status response comprising the reflected 4-byte tag plus the explicit little-endian 56-byte pinned `RepeaterStats` schema. Six positive and sixteen fail-closed negative checks must pass, and pending login, pending request, and explicit logout state must be zeroized. PR #102 banks this host-only boundary in `docs/completion/evidence/wp04/admin_host_fixture_6c096afbb5c791bf661ff146c6fbcb1f4852d2da.json` (SHA-256 `f8f7c382fb13cf1cd524678692f389b83b66c3a047c723a44f248be7a5d75283`). Concrete production packet-pool dispatch, WP-18 controlled-peer acceptance, UI, RF, hardware, and physical qualification remain open.
- Production read-only repeater-admin contract: run `python -m pytest --cache-clear -q tests/test_meshcore_admin_dispatch_contract.py tests/test_meshcore_admin_dispatch_native.py tests/test_meshcore_identity_exchange_native.py tests/test_meshcore_conformance_contract.py tests/test_meshcore_oracle_contract.py tests/test_meshcore_public_rf_contract.py`. The PR #166 gate requires 47 passing tests, exact contact/identity revalidation, bounded request/idle/absolute deadlines, redacted snapshots, zeroized authority material, volatile 8-peer by 4-response replay protection, no room or mutation support, and no hardware/RF claim. On that exact source, raw Admin TX is service-queued while Admin runtime/session parsing and state mutation remain caller-side; do not claim WP-06 sole-owner completion. PR #168 and exact-main Actions `29515366961` close the P2 direct-response derivation/session-invalidation gap: identity or shared-secret re-derivation failure must invalidate the authoritative runtime session before local copies are wiped. The final WP-18 gate still requires `admin_session_acceptance_<sha>_<d1l>_<peer>.json` with a controlled compatible peer.
- Legacy protocol-timestamp migration contract: run `python -m pytest --cache-clear -q tests/test_settings_protocol_migration_contract.py tests/test_settings_protocol_migration_native.py tests/test_time_service_checkpoint_integration_native.py tests/test_settings_contract.py tests/test_time_service_contract.py`. Inspection must never mutate NVS, interpret `mesh_ts` as wall time, or promote the predecessor lower bound automatically. Migration requires the exact confirmation string, the observed legacy value, and an operator-supplied exact-device upper bound that covers every predecessor timestamp including RAM-only fallback. Durable order is intent receipt, `mesh_hi_v2` high-water write, legacy-key erasure, then completion receipt; every modeled power-cut and NVS failure window must resume idempotently without lowering the bound. Receipt inspection is capped at 256 bytes while preserving larger future schemas, and malformed, checksum-failed, newer-schema, downgrade, revision-saturated, or storage-error states remain quarantined and TX-blocking. USB output must not log the supplied confirmation, must wipe the command buffer, and after success must query live protocol TX readiness/block reason rather than claiming readiness. PR #167 banks 33 focused tests and exact-main software/artifact integrity only. Physical legacy-state migration, authenticated companion transport, cold-boot/SNTP/jump/TLS/reboot/power-interruption behavior, and `time_service_acceptance_<sha>.json` remain required.
- Production identity startup contract: settings load must return `ESP_OK` before identity absence can be classified; unreadable settings and inconsistent identity must fail closed without generation or save, and inconsistent material must remain preserved for recovery. Generation is allowed only from a complete absent classifier and is bounded to eight candidate attempts. PR #104 banks exact-main software evidence in `docs/completion/evidence/wp04/production_identity_binding_16db6055f47541756f79edd06530d0cd1a6c878b.json` (SHA-256 `91e58c2a5ce1e1c8f847c67da401e94856e6c5266ed4bb93f8a9f1eccddf6ac7`).
- Verified-advert contact-binding contract: accept only a verified non-self signed advert, retain the exact-full-key node, reject a fingerprint-prefix collision before replay classification or mutation, and upsert the exact-key contact before incrementing accepted-advert telemetry. An equal-timestamp exact-key advert may retry missing-contact promotion only from the retained node after a transient contact-store failure. PR #105 banks exact-main software evidence in `docs/completion/evidence/wp04/verified_advert_contact_binding_e7ddb265a0a84e7ecc3860bebf959d9551fdb00a.json` (SHA-256 `0df6bdf1b1cd33883bf837f1c0e23a2df5230bf6df5af311cdd81c1e8ae7bb82`). Exact-main Actions `29348320732` passed 961 host, 32 checksum, 1,008 wire, 931 oracle, and 100,000 fuzz checks with zero findings. UI/sendability, deletion/export, capacity, reboot, power-loss, RF, and physical lifecycle proof remain open.
- Radio/runtime ownership contract: SX1262 callbacks may only copy a bounded payload, capture the monotonic timestamp, enqueue without blocking, latch terminal recovery when necessary, wake the runtime owner, and return. The runtime owner must provide a bounded priority lane, admit a normal command after at most four radio and two priority dispatches, give exact terminal events precedence, preserve a command dequeued before a terminal win, hold TX-producing commands while TX is active, defer RX recovery during TX, and expose saturating depth/high-water/drop/fairness/maintenance/recovery telemetry. PR #106 banks the callback slice, PR #109 banks runtime-owned advert admission, PR #171 banks bounded Admin command ownership, and PR #173 banks the bounded fairness/terminal-lane slice. Run the PR #173 focused contract/native set; the canonical gate must report 27 strict focused passes, while the independent review set reports 75 passes and a native `-Wall -Wextra -Werror` compile. This does not prove every remaining owner path, watchdog/stack margins, reboot/power-loss recovery, remote mutation semantics, RF, physical behavior, or WP-06 closure.
- Persisted DM delivery-state contract: schema-v6 records use immutable session IDs and state-plus-revision compare-and-swap, migrate legacy records deterministically, preserve retry attempts, mark reboot-interrupted in-flight sends truthfully, and never infer delivered state without the required ACK transition. PR #111 exact-main Actions `29355712370` and downloaded receipt `93f9717801ac7ca01a48d66d9a7c3de7acfe0cb86b0f1f5cfe78e738ed339f49` bank this storage/state slice only; runtime scheduling, timeout/retry integration, official-peer RF/DM acceptance, UI integration, and WP-07 closure remain open.
- Canonical contact-lifecycle contract: strict bounded MeshCore URI import/export validates scheme, length, UTF-8 names, role, and full key; heard-only rows cannot fabricate canonical contacts; collision/capacity errors reject before mutation; favourite/alias preferences are preserved; all DM entry points call canonical provenance authorization before identity/radio side effects; USB JSON remains parseable for quote/backslash aliases. PR #112 exact-main Actions `29359402515` and downloaded receipt `53e07c470b01a46ffcc2414c4e5b9867da9932b11203259a3d0e4e48cd3f78dc` bank this slice only; BLE/on-device QR, official-client bidirectional proof, retained reboot/refresh acceptance, physical/RF evidence, downstream channel integration, and WP-08 closure remain open.
- Autonomous-runner safety override: `scripts/autonomous_hardware_validate_d1l.py` is planning-only and rejects hardware execution before orchestration. Its legacy direct-flash and post-flash mutation paths do not yet bind the exact full D1L key or the current Pi stable target. Use only `--dry-run`; hardware evidence must use the narrow exact-candidate flash, smoke, UI, reboot, RF, and soak runners.
- UI simulator contract: `tools/ui_simulator.py` must render deterministic 480x480 PNGs plus schema-v2 `ui-sim-report.json`, cover the main touch surfaces and nested pages, fail on missing labels or measured overflow, emit 44x44 touch targets, flag RF/destructive/format-capable actions, and keep `formats_sd=false`. Storage retains its proven hierarchy. Map renders the actual current view plus `Map -> Map options -> Set location or Cache status`; its canvas fills the content region without a map-local title row, and sparse edge overlays show one-finger pan plus direct 48x48-or-larger `-`/`+`/`Center` controls, default zoom 10, the 8-through-14 range, `(c) OpenStreetMap contributors`, the visible-tile limit of 9, one zoom per visible generation, completed same-view frame reuse, and cache reuse while exposing no provider/source editor, background/prefetch, off-screen batch, or area-download action. The `map-ready` fixture must show bright, non-red, role-aware signed-advert markers with readable names below them and no ambiguous saved-center pin; `map-downloading` must visibly show determinate `Downloading n/N` progress.
- P0 UI hardware-script contract: `scripts/ui_corruption_probe_d1l.py --dry-run --rounds 20`, `scripts/ui_capture_d1l.py --dry-run`, `scripts/ui_compose_keyboard_capture_d1l.py --dry-run --targets all`, and `scripts/scroll_probe_d1l.py --dry-run --screens home,public_messages,dm_thread,nodes,packets,settings,storage,storage_card,storage_data,wifi,map,map_options,map_location,map_cache` stay host-only and require an explicit port for hardware mode. All-tab and Map probes must enter Map through the network-suppressed `ui scroll-probe` path, never an unsuppressed `ui tab map`. Hardware artifacts must include successful `map tiles status` rows before and after Map automation, prove that `network_requests` did not change, and report `network_tx=false`, `map_network_requests=false`, `background_download=false`, `area_download=false`, `visible_tile_limit=9`, and `zoom_batch_limit=1`; probes never request map tiles or mutate Wi-Fi/RF/storage.
- First-boot onboarding contract: current settings must persist `onboarding_complete`, optional manual map center, and saved Wi-Fi profile metadata while migrating older settings without dropping identity. No provider URL, API key, attribution editor, or user-selected tile source is part of onboarding or settings.
- Map location contract: normal Map entry opens the actual Map and never auto-opens an editor. `Options -> Set location` opens decimal latitude/longitude fields with an onscreen keyboard; Back makes no change, Save persists a valid center, and Clear is available only when a saved center exists. Serial `map center set|clear` commands share the same persistence path without Public RF, tile networking, SD writes, or formatting.
- Map interactive-fetch and signed-location contract: canonical surfaces are `map|map_options|map_location|map_cache`. OpenStreetMap Standard is built in. The saved center opens at default zoom 10; direct `-`/`+` controls clamp to zooms 8 through 14, `Center` returns to the saved location, and a one-finger drag commits a pan only on release. Only while the actual Map is visible may firmware request the visible current-view 3x3 at one zoom per visible generation, selected by the user. If the complete persistent-file gate is not ready when a physically opened Map begins, that same visible generation waits on cached storage status with zero tile attempts or network requests and resumes automatically when SD becomes ready; hiding or covering Map cancels the wait. Drag motion must not request tiles. Hidden Map cancels unfinished work. Passive marker refresh must query at most 32 newest located nodes and display at most eight. Only a verified, nonfuture MeshCore signed advert with a valid location may supply a pin; the detail label must say `Advert location`, report verified age and exact known role, retain unknown accuracy, and never infer expiry, local GPS, or precision. Initial entry, interactive pan/zoom, and Center reacquisition must use explicit center provenance and fail closed if it is absent or invalid. Trust loss invalidates the retained view and releases the lease; backward wall-time correction rechecks future pins. Markers remain bright non-red role-aware dots with readable below-marker names, skip deterministic collisions/control/attribution exclusions, and update only their lightweight layer without acquiring a tile generation or copying/repainting the tile frame. The marker layer follows drag preview and reprojects only after committed pan/zoom. A viewport-level hit radius of 22 px opens Node Detail by retained fingerprint; Close reacquires the unchanged retained Map view without a new intentional download. Run `python -m pytest -q tests/test_map_marker_truth.py tests/test_map_point_projection.py tests/test_map_viewport_ui_contract.py`; the canonical PR #175 focused gate records 23 passes. A completed exact-view Home-to-Map revisit must retain the frame revision/generation and leave attempted/cache-hit/download/network counters unchanged, with no SD read operation in the trace. A reboot may reread the SD tile cache but must not redownload cached tiles. Tile cache/reuse remains bounded to the current view. There is no background fetch and no area download; multi-zoom prefetch and off-screen batches are also forbidden. `(c) OpenStreetMap contributors` remains visible. Probes never request map tiles. This bounded software gate does not close physical/RF behavior, provider qualification, the required WP-16 artifact, WP-16, or release.
- Phase 6 packet filter/raw-hex contract: packet log entries must carry a bounded raw hex preview, expose `packets filter`, `packets search`, `packets raw`, and render Packet-tab filter/search/raw-hex UI surfaces in the simulator.
- Live RF receive UI stability contract: packet, Public message, DM, node, route, contact, and mesh-inspector stores must serialize RAM ring/static scratch reads and writes so UI snapshots cannot copy partially mutated packet-receive state. The periodic UI refresh timer may only update chrome/status and queue a coalesced content refresh when message, packet, node, route, or contact generation counters change; active page redraw must happen later on the UI task, outside LVGL event callbacks. While the actual Map is active, those unrelated counters and transient SD busy/readiness changes must be acknowledged without tearing down its viewport, drag, or unfinished tile batch. Only a changed saved location/default zoom may request a full Map rerender; a physically opened Map that is waiting for SD resumes the same generation when the cached full file gate becomes ready.
- MeshCore service ownership contract: `d1l_meshcore_service_status()` and UI/app-model snapshot polling must be passive cached-status reads only; they must not generate identity, save settings, initialize/configure radio, call RX, or send RF. Explicit serial/touch commands such as advert, Public send, DM send, and trace probe must enter the MeshCore service task/queue before radio start/RX/TX so the service remains the radio owner.
- Phase 6 mesh visibility contract: `signal`, `roomservers`, and `repeaters` must be machine-readable, read from bounded packet/route/node stores, avoid new NVS writes, and appear in the smoke command list.
- Phase 6 contact export contract: promoted contacts with retained 64-hex public keys and canonical chat/repeater/room/sensor roles must export MeshCore-compatible `meshcore://contact/add?...` URIs through serial and a touch Contact Export QR sheet. Unknown/malformed roles fail closed and render Export unavailable; the smokeable list form still succeeds when no contact is available.
- Phase 6 radio settings contract: `settings get` and `radio get` must expose the persisted radio profile plus `applied_to_radio`, `radio_apply_pending`, and `radio_apply_error`; serial `radio set txpower` and `radio set rxboost` must validate and persist safe values while truthfully reporting whether live RF already matches the saved profile, and the Settings tab must open a simulator-covered Radio Settings sheet with staged edits, US/CAN defaults, explicit Save, and RF apply status.
- Optional SD-card data storage contract: `rp2040 ping` must prove the flashed RP2040 bridge app answers without touching SD (`sd_touched=false`, `public_rf_tx=false`, `formats_sd=false`) before SD-specific checks. `storage status` must be safe for boot/UI polling and must not probe, mount, format, or write SD; before an explicit mount it may report `state="mount_required"`, and after a mount/file result it must report the cached fallback backend, direct ESP32 SD support, RP2040 bridge/protocol/card/root state, per-store backend labels, setup action, retained SD health counters, and cached bridge probe fields such as `probe_power`, `probe_mode`, `probe_error`, `probe_data`, `mount_error`, and `mount_data`. `storage mount` is the explicit direct SD-touch path; it may mount a usable FAT32 filesystem and create `/deskos/manifest.json` plus required DeskOS directories, must report `public_rf_tx=false` and `formats_sd=false`, and must not format. `storage status` must expose a storage-manager object with `running`, `state`, `attempt`, `backoff_ms`, and `force_nvs`; `storage remount`, `storage reset-bridge`, and `storage force-nvs [on|off]` must remain non-formatting and non-RF while allowing the manager to converge through `BRIDGE_WAIT`, `PING`, `STATUS`, `MOUNT`, `READY_SD`, `READY_NVS`, `NEEDS_FAT32`, `NO_CARD`, and `ERROR_BACKOFF`. Reboot/remount acceptance must reject every remount error. An initial manager-busy response is diagnostic only: acceptance must wait for a later fresh `READY_SD` sample, retry remount once, and require that successful retry to report `retained_worker_quiesce_acquired=true` before any canary/readback command; cached ready fields cannot close the gate. Unmountable cards must report `needs_fat32`/`prepare_fat32_on_computer` style guidance and keep NVS fallback active; when a card is independently confirmed FAT32, that result is a firmware-side mount blocker until resolved. `storage diag` must issue only the non-formatting `DESKOS_SD_DIAG` probe and report high/dedicated, high/shared, low/dedicated, and low/shared results with `public_rf_tx=false` and `formats_sd=false`. `storage diag raw` must preserve the full RP2040 diagnostic line for bitbang, inverted-CS, swapped-pin, CMD0/CMD8, R7, and MISO-token debugging without formatting. `storage setup` must be non-destructive and report `policy="no_device_format"`. The RP2040 bridge file protocol must preserve the `DESKOS_SD_FILE v=1` line grammar, sanitized relative paths, CRC32-checked base64url payloads, 512-byte line cap, and 192-byte chunk cap. Retained Public/DM message history, route history, and packet history may report `sd` backends only when the bridge reports ready data, file operations, matching limits, and atomic rename; retained-history and reboot/remount evidence must re-prove `data_backend="mixed"` plus Public/DM/route/packet `sd` backends after reboot, because readable NVS mirrors are not SD-retention proof. NVS remains mirrored as fallback, `retained_sd.stores.*` must expose `sd_read_fail_count`, `sd_write_fail_count`, `sd_rename_fail_count`, `sd_last_error`, `sd_degraded_latched`, and `nvs_mirror_fail_count`, and the UI/serial status must show `SD degraded; using internal fallback` once a real SD failure latches. Diagnostic and sampled data exports may report `export_backend="sd_diagnostic_exports_ready"` only when the same file-operation gate is ready, and map tile cache may report `map_tile_backend="sd_map_tiles_ready"` behind that same file gate. The synthetic `storage map-tile-canary <token>` validates only the cache file path and never performs network I/O. Production Map networking is a separate guarded current-view path; there is no arbitrary URL/template download command.
- RP2040 bridge preflight contract: `scripts/rp2040_sd_bridge_preflight_d1l.py --port <D1L_PORT> --artifact-dir <rp2040-sd-bridge-firmware>` must verify the Actions-built UF2 artifact when supplied, list UF2 bootloader candidate volumes, query only `rp2040 status`, `rp2040 ping`, safe `storage status`, explicit non-formatting `storage mount`, bounded safe-status polling while SD is `mount_pending` or the storage manager is in `BRIDGE_WAIT`, `PING`, `STATUS`, or `MOUNT`, optional `storage diag`, and `health` on the selected D1L serial port. `ready_for_sd_acceptance=true` requires both the existing fresh FAT32 file-operation gate and `manager.state="READY_SD"`; an exhausted transitional state must set top-level `ok=false`, return nonzero, and remain fail-closed even when `serial_commands_ok=true`. The receipt and stdout summary must expose acceptance plus settle exhaustion. Total status polls, SD-mount-pending polls, and manager-transition polls are counted separately, with initial/final manager state and a machine-readable `wait_for_storage_manager_ready_sd` next action. Terminal non-ready states such as `no_card` remain successful diagnostics rather than settle-exhaustion failures. The receipt must also emit `public_rf_tx=false`, `formats_sd=false`, `copies_uf2=false`, `rp2040_ping_ok`, `storage_mount_ok`, and `storage_diag_ok`.
- Retained-worker remount contract: serial `storage remount` and every background storage-manager run must acquire the storage-manager sequence before a bounded owner-safe retained-worker quiesce, exclude periodic and forced Public/DM/packet/route persistence throughout bridge status/mount/reset exchanges, and release retained-worker quiesce before the manager sequence. Serial remount spends its existing absolute deadline while waiting for a background sequence owner, then acquires retained-worker quiesce from the remaining budget; sequence timeout returns before any worker or bridge operation. Successful remount reports `retained_worker_quiesce_acquired=true`. A background status exchange failure must disable retained SD before releasing the worker and keep `manager.state="STATUS"` while cached status is stale; only a fresh reply under the same quiesce may re-enable the backend and advance its generation. Reboot keeps one deadline and the order storage-manager quiesce, forced retained flush, retained-worker quiesce, then RP2040 bridge quiesce; failure unwinds in reverse, while success reports `retained_worker_quiesced=true` and holds all three quiesces through restart. The packet store upgrades only an exact, structurally valid schema-v2 eight-entry SD compact blob into the schema-v3 primary. Two individually valid but disjoint schema-v3 SD and device-local histories are exact-deduplicated, order-preserved within bounded capacity, deterministically resequenced, and rewritten under the same backend generation before journal append resumes at a fresh segment; same-sequence different payloads remain separate evidence. A malformed or internally gapped current-schema blob remains fail-closed. SD boot-prepare acceptance must fail immediately on remount receipts without the acquired flag and on any retained degraded/backup-degraded flag, per-store SD/NVS counter, non-`ESP_OK` retained error, or degradation latch, including the existing-data scenario.
- SD boot/use acceptance contract: `scripts/sd_boot_prepare_acceptance_d1l.py --port <D1L_PORT> --scenario <scenario>` must cover `no-card`, `correct-structure`, `missing-structure`, `unformatted`, `existing-data`, and `rp2040-unavailable` without hardcoded ports, Public RF, or any formatting command. Users must supply FAT32 cards prepared on a computer.
- Packet-journal continuation is trusted only when a read-only probe proves that the journal tail payload exactly equals the compact-primary tail and the next deterministic slot is `NOT_FOUND` or the bridge's exact zero-byte EOF. An occupied, partial, malformed, or payload-mismatched slot is preserved and defers journaling to the next segment boundary; transport and backend-generation errors fail before any primary, fallback, or journal mutation. The boundary write may truncate only its selected cyclic segment.
- Phase 2 MeshCore service command surface.
- Phase 4 Public message store contract including retained-history search, DM store contract including thread-filtered retained history, unread/read-state contract including per-thread DM read cursors, heard-node store contract, contact store contract, route store contract, persistent packet log contract, Public composer UI contract, and serial diagnostics.

- WP-01 diagnostic-isolation contract: run asynchronous `storage diag raw` only in an isolated maintenance boot and do not begin retained canaries after a diagnostic merely because an additional 750 ms ordinary timeout elapsed. After diagnostic capture, reset and reflash the checksum-verified exact Actions artifacts, then require a clean `READY_SD` preflight with zero retained read/write/rename failure counters before file, retained, reboot, inserted-card, remove/reinsert, or soak evidence begins. Any overlap with the one-second retained worker or any pre-canary counter contamination invalidates that evidence window.

## Issue #63 Supported-SDK Qualification

Firmware compilation and ESP-IDF dependency resolution are GitHub-Actions-only.
Do not run a local firmware build and do not hand-edit `dependencies.lock`.

1. Run the complete host suite and confirm the workflow-policy tests reject
   missing, moving, EOL, and unapproved SDK image tags while accepting only
   `espressif/idf:v5.5.4` as the selected target.
2. Run the version-pinned Actions firmware job. Archive the generated
   `dependencies.lock` and its diff, plus any generated configuration diff,
   even if the first migration build fails later.
3. Review and commit the exact Actions-generated lock. Rerun the same commit
   lineage in Actions and require the firmware/package/checksum jobs to pass
   without changing the committed lock or silently changing generated config.
4. Retain the commit, run ID, selected tag, resolved image identity when
   available, lock, checksums, and downloaded firmware/package metadata.
5. Flash only that verified Actions artifact to the qualified D1L target. On
   the current route, the hardware flash receipt must bind `neopi5`, the exact
   stable by-id selector, and USB VID:PID `1A86:7523`; on the Windows
   alternative it must bind `COM12`. It must match the selected full commit and
   numeric Actions run, prove successful non-erasing `esptool write-flash`, and
   bind the exact
   command offset/file set to the complete checksummed artifact. It must include
   the D1L bootloader at `0x0`, partition table at `0x8000`, and application at
   `0x10000`, with no extra pair. Run `version` and require an OK JSON response
   whose `idf` is exactly `v5.5.4` and whose full `build_commit` equals the
   selected 40-hex commit.
6. On the same artifact, repeat issue #63 board, display/touch, Wi-Fi, RF,
   RP2040/SD, Map, health, reboot, and post-power-cycle smoke. Use COM16 only
   when an RP2040-specific proof requires it; do not substitute any other
   serial device or a raw `/dev/ttyUSB*` path for the qualified D1L target.
7. Refresh all relevant commit-matched release-gate evidence. Qualified-target
   smoke must
   record the same full commit in `expected_firmware_commit`,
   `device_build_commit`, and the `version` result. A run using
   `--skip-esp32-flash` intentionally leaves `exact_actions_esp32_flash` failed.
   The SDK migration passes only when `supported_sdk_baseline` and every other
   P0 gate are green.

## WP-01 Exact-Pair Evidence Checkpoint

WP-01 is merged with `proof_banked=true`. Its accepted physical source remains `092293f2311a24c9899bc9bf343ab014c4ba0411`; exact push/PR Actions runs are `29272708844` / `29272709642`. The Actions host job reports **773 passed**, and checksum verification covers **8 manifests / 78 entries**. This evidence remains predecessor-bound and must not be relabelled as successor proof.

| Canonical artifact | SHA-256 | Accepted result |
|---|---|---|
| `wp01_exact_pair_provenance_092293f.json` | `2decf8ad60b73e71bbb09b489adba8fd827856a8daf00c376c5a9ba5354e451e` | Exact ESP32/RP2040/Actions pair and checksum provenance passed. |
| `sd_inserted_stability_092293f_COM12_COM16.json` | `a038ee7ca371c4ee404493c721a343ef54e7bbd55b08273c8dc833d9d0203aef` | Inserted-card stability passed. |
| `sd_remove_reinsert_092293f_COM12_COM16.json` | `3a3882038fec2497529d281f3c2b9b7468c1e62dcca7962ca3b9492125f0fad1` | 10/10 physical removal/reinsert cycles passed. |
| `retained_reboot_matrix_092293f_COM12.json` | `db6cab3020bfa8ef575bd6a59c61d1277a8e72e1e73eb987248817199797a986` | 5/5 retained reboot cohorts passed. |
| `storage_active_soak_092293f_COM12_COM16.json` | `caf19395d0e1a175f6fa13c2550bc8693297661756bf339ba4acec63da2699b9` | 7,207.089 seconds, six segments, retained-worker stack floor 7,976 bytes. |
| `wp01_acceptance_092293f_COM12_COM16.json` | `994f4e5ac7b9e0e8bdb57aad7715f52a99294a1841847860e2ce2f70bd6e2277` | Aggregate passed with all required source artifacts bound. |

All accepted WP-01 evidence reports `public_rf_tx=false` and `formats_sd=false`. It closes only the narrow PR #80 source storage/reboot gate. It does **not** close the later exact integrated/frozen-candidate no-card, unusable/non-FAT32, representative-card/size, Seeed, electrical, power-loss, cold/warm boot, 12-hour idle/listening, UI, Map, or RF matrices.

PRs #62, #64, #80, #84-#99, and #101-#112 are merged on live checkpoint `10d85ee3a0941aff23f455047358805a861b571e`. Exact-main Actions `29359402515` pass 977 host plus 32 checksum tests and verify five API ZIPs / 46 entries across 219 files / 73,155,041 bytes, with 100,000 wire and 100,000 advert fuzz executions and zero findings. Strict receipt SHA-256 is `53e07c470b01a46ffcc2414c4e5b9867da9932b11203259a3d0e4e48cd3f78dc`. Exact-head Actions dispatch `29296995585` remains the pinned RP2040 build checkpoint. Complete semantic/replay/state coverage, remaining contact lifecycle, admin dispatch, generic TRACE, remaining WP-06/WP-07/WP-08/WP-14 ownership/recovery, RF, and physical closure remain open.

For the current contact-targeted TRACE software regression, run `python -m pytest --cache-clear -q tests/test_meshcore_trace_contract.py tests/test_meshcore_trace_native.py tests/test_meshcore_admin_dispatch_contract.py tests/test_meshcore_route_selection_contract.py tests/test_meshcore_oracle_contract.py::test_oracle_manifest_is_exactly_pinned_and_fail_closed`. This focused subset requires 22 passing tests; the complete retained PR #168 gate requires 68 focused TRACE/Admin tests. Acceptance requires request `flags=0` for one-byte contact hashes or `flags=1` for two-byte contact hashes, generic wire parsing/building for pinned `flags=[0,1,2,3]` / `trace_wire_hash_bytes_supported=[1,2,4,8]`, fail-closed rejection of unrepresentable three-byte contact routes, one pending request, a 30-second timeout, a 60-second duplicate window, correlation on tag plus the opaque correlation code plus the exact immutable runtime-derived loop and hash width, immediate Admin-session invalidation on direct-response derivation failure, fixed `trace_last` retention, `contact_trace_supported=true`, `operator_path_accepted=false`, `one_byte_hash_only=false`, `contact_trace_path_hash_bytes_supported=[1,2]`, and `hardware_verified=false`. This host check sends no RF and cannot close physical or official-peer gates.

For the merged PR #103 software-only identity/contact regression, run `python -m pytest --cache-clear -q tests/test_identity_state_contract.py tests/test_identity_state_native.py tests/test_verified_contact_contract.py tests/test_contact_store_contract.py`. Acceptance is 8 passing tests and the fail-closed classification, exact-full-key authority, collision/capacity behavior, rollback, preference/path preservation, and reload semantics captured by the portable aggregate. This host check does not exercise production signed-advert RX, UI, RF, or hardware.

For the WP-15 Messages software-behavior checkpoint through PR #150, run `python -m pytest --cache-clear -q tests/test_ui_messages_contract.py tests/test_ui_dm_conversation_contract.py tests/test_ui_dm_search_contract.py tests/test_ui_compose_eligibility_contract.py tests/test_ui_compose_eligibility_native.py tests/test_ui_dm_identity_contract.py tests/test_ui_dm_identity_native.py tests/test_ui_message_states_contract.py tests/test_read_state_contract.py tests/test_read_state_cursor_invariants_native.py tests/test_ui_message_cursor_refresh_contract.py tests/test_ui_home_view_contract.py tests/test_ui_home_view_native.py tests/test_ui_simulator.py`. Acceptance requires exact-key DM eligibility to fail closed, truthful loading/degraded/unavailable/no-contact/failure/retry states, readable retained history during storage faults, idempotent transactional Public and exact-DM read cursors with RAM rollback on persistence failure, muted-unread separation, and incoming Public/DM refresh that preserves mode, overlays, search and compose focus while initiating no RF. Exact main `c20d5b4580188fe86d1607845050f08aa37fec24` / Actions `29438968424` banks this software boundary; controlled-peer RF, exact-candidate physical touch/keyboard/scroll/UTF-8, dependency closure, and `messages_ui_acceptance_<sha>_<d1l>.json` remain mandatory. Curated emoji/glyph/notification/accessibility/polish is WP-21 scope.

For the merged PR #102 software-only admin-fixture regression, initialize the pinned SenseCAP submodule and run `python -m pytest --cache-clear -q tests/test_meshcore_oracle_contract.py`. Acceptance is 15 passing tests, the pinned upstream schema hash, six positive and sixteen fail-closed negative transcript checks, direct/flood PATH-return behavior, and session-state zeroization. This host check does not exercise production admin dispatch, WP-18, UI, RF, or hardware.

For WP-02 physical closure, take no device action while release-security and protocol commits are still moving the SHA. After the final candidate is frozen, install its checksum-verified exact ESP32/RP2040 pair and collect only board boot/runtime on COM12, UI workflow on COM12, SD path on COM12/COM16, retained reboot on COM12/COM16, and Map-open on COM12. Each receipt must bind that exact SHA, its exact successful Actions run and artifact hashes, the applicable explicit ports, and canonical booleans `physical_observed=true`, `dry_run=false`, `simulated=false`, `simulation=false`, `source_inspection=false`, `public_rf_tx=false`, `dm_rf_tx=false`, and `formats_sd=false`; strings or omitted fields do not qualify. Never use COM8, COM11, or COM29 and never format SD. Until all five exist, the tracked portable baseline `docs/completion/evidence/wp02/integration_baseline_4ee07caf09906abdcebe8faccd95790dceb5fe88.json` remains `ok=false`; this does not block host-only WP-03/WP-04 execution, and it cannot itself close a later candidate.

Downloaded release-package verification is recursive and fail-closed. The top `SHA256SUMS.txt` must cover every package file except itself, including each copied RP2040 bundle's nested `SHA256SUMS.txt`; each nested manifest must independently verify its own files. Exact-main run `29286754864` remains the preserved 7/8 negative receipt. PR #64 closed the defect; successor exact-main run `29290978741` strict-passed 8 manifests / 78 entries with root manifest SHA-256 `22e554bef7988f4132bd0bccc5657bb617035d1a8a9beab7c4c7b717e5e79b64`.

## Hardware Smoke

Run only after the target checks above pass. For issue work, do not run this
whole block. Pick the one proof that closes the selected P0, attach that
artifact to the issue/PR, and move on to the next blocker.

For the current Pi route, flash only from the extracted checksum-verified
exact Actions package:

```bash
cd /path/to/checksum-verified-actions-package
./flash_project.sh
cd /path/to/repository-checkout
python ./scripts/smoke_d1l.py --port "$D1L_PORT" --manual-touch
```

For the Windows alternative only:

```powershell
$env:D1L_PORT = "COM12"
& <checksum-verified-actions-package>\flash_project.ps1 -Port $env:D1L_PORT
python .\scripts\smoke_d1l.py --port $env:D1L_PORT --manual-touch
```

This release run does not create a flash backup. The backup helper is an
optional recovery tool for a separately authorized session, not a production
flash prerequisite.

Historical/Windows-alternative COM12 proof templates follow. For current Pi
evidence, pass the stable `$D1L_PORT`, write under an explicit `neopi5`
evidence directory, and never use `COM12` in the filename or identity fields:

```powershell
# Split/stale redraw proof.
python .\scripts\ui_corruption_probe_d1l.py --port $env:D1L_PORT --rounds 20 --clear-crashlog-before-start

# Hardware pixel / Home-screen proof.
python .\tools\ui_simulator.py --view home --out artifacts\ui-sim-reference\current
python .\scripts\ui_capture_d1l.py --port $env:D1L_PORT --prep-command "ui tab home" --reference-png artifacts\ui-sim-reference\current\home.png --reference-view home --out artifacts\hardware\com12\ui_pixel_capture-COM12.json

# Safe nested contact-page proofs. These targets only open pages. In particular,
# contact_forget displays the warning without invoking its red confirmation callback.
python .\scripts\ui_capture_d1l.py --port $env:D1L_PORT --prep-command "ui scroll-probe contact_detail" --reference-png docs\screenshots\contact_detail_sheet.png --reference-view contact_detail_sheet --out artifacts\hardware\com12\ui_pixel_capture_contact_detail-COM12.json
python .\scripts\ui_capture_d1l.py --port $env:D1L_PORT --prep-command "ui scroll-probe contact_options" --reference-png docs\screenshots\contact_options_page.png --reference-view contact_options_page --out artifacts\hardware\com12\ui_pixel_capture_contact_options-COM12.json
python .\scripts\ui_capture_d1l.py --port $env:D1L_PORT --prep-command "ui scroll-probe contact_forget" --reference-png docs\screenshots\forget_contact_confirm_page.png --reference-view forget_contact_confirm_page --out artifacts\hardware\com12\ui_pixel_capture_contact_forget-COM12.json

# Safe Mesh Roles page-open proofs. These aliases only navigate to read-only pages.
python .\scripts\ui_capture_d1l.py --port $env:D1L_PORT --prep-command "ui scroll-probe mesh_roles" --reference-png docs\screenshots\mesh_roles_sheet.png --reference-view mesh_roles_sheet --out artifacts\hardware\com12\ui_pixel_capture_mesh_roles-COM12.json
python .\scripts\ui_capture_d1l.py --port $env:D1L_PORT --prep-command "ui scroll-probe mesh_rooms" --reference-png docs\screenshots\mesh_rooms_page.png --reference-view mesh_rooms_page --out artifacts\hardware\com12\ui_pixel_capture_mesh_rooms-COM12.json
python .\scripts\ui_capture_d1l.py --port $env:D1L_PORT --prep-command "ui scroll-probe mesh_repeaters" --reference-png docs\screenshots\mesh_repeaters_page.png --reference-view mesh_repeaters_page --out artifacts\hardware\com12\ui_pixel_capture_mesh_repeaters-COM12.json

# Safe Storage page-open proofs. These aliases only navigate through read-only pages.
python .\scripts\ui_capture_d1l.py --port $env:D1L_PORT --prep-command "ui scroll-probe storage" --reference-png docs\screenshots\storage_setup_sheet.png --reference-view storage_setup_sheet --out artifacts\hardware\com12\ui_pixel_capture_storage-COM12.json
python .\scripts\ui_capture_d1l.py --port $env:D1L_PORT --prep-command "ui scroll-probe storage_card" --reference-png docs\screenshots\storage_card_page.png --reference-view storage_card_page --out artifacts\hardware\com12\ui_pixel_capture_storage_card-COM12.json
python .\scripts\ui_capture_d1l.py --port $env:D1L_PORT --prep-command "ui scroll-probe storage_data" --reference-png docs\screenshots\storage_data_page.png --reference-view storage_data_page --out artifacts\hardware\com12\ui_pixel_capture_storage_data-COM12.json

# Network-suppressed Map page-open proofs from the exact Actions artifact.
# The script must not issue `ui tab map` and the artifact must report no tile requests.
python .\scripts\scroll_probe_d1l.py --port $env:D1L_PORT --screens map,map_options,map_location,map_cache --out artifacts\hardware\com12\scroll_probe_map-<sha>-COM12.json

# Compose-keyboard proof.
python .\scripts\ui_compose_keyboard_capture_d1l.py --port $env:D1L_PORT --targets all --out artifacts\hardware\com12\ui_compose_keyboard_capture-COM12.json

# One scroll/layout surface, or a tiny list named by the issue.
python .\scripts\scroll_probe_d1l.py --port $env:D1L_PORT --screens <screen-or-small-list> --manual-touch --clear-crashlog-before-start
# The Clear flag is rejected for every Map surface; use the network-suppressed Map command above.
```

For a deliberate current-target UI sweep after the matching GitHub Actions artifacts are
downloaded:

```powershell
python .\scripts\autonomous_hardware_validate_d1l.py --github-run-id <run-id> --github-run-dir artifacts\github\<run-id>-current --commit <sha> --skip-sd-suite --include-ui-probes --dry-run
```

That sweep requires no manual touch confirmation, does not open COM8, COM11, or
COM29, does not copy RP2040 UF2 files, reuses the already-validated production
bridge, skips the SD suite, and writes the final release-gate audit under
`artifacts\release-gate`. Unless `--skip-esp32-flash` is explicitly supplied,
it first verifies and flashes the selected Actions artifact and writes the
commit/run/qualified-target receipt required by the P0 audit; smoke then rejects any device
whose reported full build commit differs. It can still take time because it
cycles multiple UI surfaces; do not use it as the default proof for a
one-surface P0.

Only when RP2040 bridge firmware, SD smoke firmware, or SD electrical evidence
actually needs refreshing, opt into the UF2 path:

```powershell
python .\scripts\autonomous_hardware_validate_d1l.py --github-run-id <run-id> --github-run-dir artifacts\github\<run-id>-current --commit <sha> --refresh-rp2040-smoke --dry-run
```

That refresh path copies only Actions-built RP2040 UF2 artifacts, captures the
RP2040-unavailable fallback window after official SD smoke, restores the
production bridge, then checksum-verifies and non-erasingly reflashes the exact
ESP32 image before the strict pre-diagnostic clean gate. This fresh boot clears
failure latches intentionally observed while the smoke firmware made the bridge
unavailable; a failed reflash stops before preflight. The runner then executes
the qualified-target SD canary suite.

Expected commands:

- `version`
- `board`
- `settings get`
- `settings onboarding status`
- `identity status`
- `i2c`
- `display test`
- `touch test`
- `button`
- `radiohw`
- `radio get`
- `map center`
- `mesh status`
- `companion status`
- `wifi status`
- `wifi scan`
- `ble status`
- `rp2040 status`
- `storage status`
- `storage map-policy`
- `storage setup`
- `packets`
- `packets filter any any`
- `packets search test`
- `messages public`
- `messages public offset 8`
- `messages public search test`
- `messages public search test offset 8`
- `messages dm offset 8`
- `messages dm <fingerprint> offset 8`
- `messages unread`
- `nodes`
- `contacts`
- `contacts export`
- `routes`
- `routes trace 0BF0A701D5AE2DB6`
- `signal`
- `roomservers`
- `repeaters`
- `crashlog`
- `health`

Hardware success must include a current qualified-target `ui_capture_d1l.py`
PNG for display pixels with a passing `ui_capture_simulator_diff` against the
matching simulator/reference view, a current qualified-target
`ui_compose_keyboard_capture_d1l.py --targets all` artifact with every
release-blocking compose/input keyboard PNG/RGB565 capture, plus manual touch
confirmation until touch automation is expanded. For the current route, every
receipt must bind `neopi5`, the stable by-id selector, and `1A86:7523`; a raw
`/dev/ttyUSB*` value is non-qualifying.

RF automation policy: transmit only through a controlled direct-message peer or
the dedicated `#test` channel. Any later step that names `mesh send public`
must first prove that `#test` is the selected channel; until #67 provides that
selection, use the DM equivalent and do not transmit on the default Public
channel. Never use a forbidden local serial port for either the D1L or its
controlled peer.

## Hardware Soak

Use the soak runner for Phase 7 stability evidence after smoke passes. The
runner writes a JSON artifact under `artifacts/soak` unless `--out` is
supplied. On `neopi5`, retain the stable `$D1L_PORT` established above; do not
replace it with the observed raw tty. Active RF soak remains blocked until the
narrow local controlled-peer access prerequisite is proven.

Short active RF probe through a promoted controlled DM peer. Automated traffic
must use DM or the dedicated `#test` channel, never the default Public channel:

```powershell
$env:D1L_PORT = "COMx"
$env:D1L_DM_FINGERPRINT = "<16-hex-controlled-peer>"
python .\scripts\soak_d1l.py --port $env:D1L_PORT --duration-sec 180 --sample-interval-sec 45 --active-dm-fingerprint $env:D1L_DM_FINGERPRINT --active-dm-text test --active-interval-sec 60 --require-rx-delta --min-tx-delta 1
```

Full idle/listening acceptance window:

```bash
export D1L_PORT='/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
export D1L_COMMIT='<exact-40-hex-Actions-candidate>'
export D1L_ACTIONS_RUN='<numeric-run-id>'
export D1L_ACTIONS_ATTEMPT='<numeric-run-attempt>'
export D1L_PUBLIC_KEY='<exact-64-hex-retained-D1L-public-key>'
python ./scripts/soak_d1l.py \
  --port "$D1L_PORT" \
  --expected-firmware-commit "$D1L_COMMIT" \
  --expected-d1l-public-key "$D1L_PUBLIC_KEY" \
  --github-run-id "$D1L_ACTIONS_RUN" \
  --github-run-attempt "$D1L_ACTIONS_ATTEMPT" \
  --expected-release-profile full_feature \
  --expected-sd-history-mode conditional \
  --duration-sec 43200 \
  --sample-interval-sec 300 \
  --out "artifacts/soak/full-feature-idle-12h-${D1L_COMMIT}-neopi5-by-id.json"
```

This invocation is release-bound. The runner rejects raw ttys, validates the
stable `1A86:7523` target before and after the run, binds the exact retained
D1L public key, and requires a clean checkout of the exact firmware commit.
The idle soak does not run an SD file canary; conditional-SD acceptance is a
separate bounded gate and is not repeated for 12 hours.

Full active messaging acceptance window through that controlled DM peer:

```powershell
$env:D1L_PORT = "COMx"
$env:D1L_DM_FINGERPRINT = "<16-hex-controlled-peer>"
python .\scripts\soak_d1l.py --port $env:D1L_PORT --duration-sec 3600 --sample-interval-sec 60 --active-dm-fingerprint $env:D1L_DM_FINGERPRINT --active-dm-text test --active-interval-sec 120 --require-rx-delta --min-tx-delta 1 --clear-crashlog-before-start --out artifacts\soak\d1l-soak-active-1h-COMx.json
```

Success requires every sampled command to return `ok=true` after bounded retries, no unrecovered command retries, no uptime rollback, `board_ready=true`, `ui_ready=true`, ready mesh state, nonzero task stack watermarks, zero crash-like reset entries, and no required packet delta threshold failures. For active RF probes, `mesh_tx_packet_delta` must increase and `mesh_rx_packet_delta` must increase when `--require-rx-delta` is used.

SD-aware passive validation, with no Public RF and no format request:

```powershell
$env:D1L_PORT = "COM12"
python .\scripts\soak_d1l.py --port $env:D1L_PORT --duration-sec 300 --sample-interval-sec 60 --sample-storage --sd-file-canary --allow-sd-unavailable --out artifacts\soak\d1l-passive-soak-sd-aware-COM12.json
```

Before the RP2040 bridge file protocol is flashed, success may include `sd_file_canary_unavailable_count > 0` only because `--allow-sd-unavailable` was set. After the RP2040 bridge is flashed with a ready card, rerun without `--allow-sd-unavailable`; success then requires `storage_file_ops_ready_all=true` and every `storage filecanary` sample to pass.

## Release Package

After downloading the matching GitHub Actions ESP32 firmware build artifact:

```powershell
python .\scripts\package_release_d1l.py --build-dir build --out-dir artifacts\release --package-name d1l-release-local-smoke
```

Verify:

1. `manifest.json` lists bootloader offset `0x0`, partition offset `0x8000`, and app offset `0x10000`.
2. `firmware/meshcore_deskos_d1l.bin` and `update/meshcore_deskos_d1l-app.bin` have matching SHA256 hashes.
3. `full-flash/meshcore_deskos_d1l-full-8mb.bin` is exactly 8MB.
4. `SHA256SUMS.txt` includes every package file except itself.
5. `flash_project.ps1`, `flash_project.sh`, and `flash_full_8mb.ps1` require an explicit D1L port.
6. `flash_full_8mb.ps1` requires typed confirmation.

## Message Store Persistence

For Phase 4 Public message-store validation:

This case is dormant until #67 can select and prove the dedicated `#test`
channel. Do not run it against the default Public channel.

1. Run `messages clear`.
2. Select `#test`, verify the selected-channel identity, then run `mesh send public test`.
3. Wait for a local MeshCore bot response.
4. Verify `messages public` contains at least one TX row and one RX row.
5. Verify `messages public search test` returns `filtered=true` and only retained rows whose author, direction, or text matches `test`.
6. When more than one serial page is retained, verify `messages public offset 8` and `messages public search test offset 8` report page metadata (`offset`, `page_size`, `total_matches`, `has_older`, `next_offset`) and return older retained rows in chronological order.
7. Reboot.
8. Verify `messages public` retains the rows and `packets` either retains the newest packet evidence rows or starts a new evidence window if `packets clear` was run for the packet-log test.

## Unread State

For Phase 4 unread/read-state validation:

1. Run `messages read all`.
2. Verify `messages unread` reports `public_unread=0`, `dm_unread=0`, and `muted_dm_unread=0`.
3. Only in the gated `#test` case above, run `mesh send public test`; otherwise use the DM-thread read-state case below.
4. Wait for a local MeshCore bot response.
5. Verify `messages public` contains fresh RX rows with seq values greater than the baseline `newest_public_rx_seq`.
6. Verify `messages unread` reports `public_unread` greater than zero and advances `newest_public_rx_seq`.
7. Run `messages read public`.
8. Verify `messages unread` reports `public_unread=0`.
9. Reboot.
10. Verify `messages unread` still reports `public_unread=0` and `health` reports `board_ready=true` and `ui_ready=true`.
11. For DM-thread read-state validation, when a DM thread has an inbound RX row, run `messages unread`, note the thread entry under `dm_threads`, run `messages read dm <fingerprint>`, and verify only that thread's unread count clears while other unread DM threads remain counted.
12. For physical touch review, open the Messages tab, verify new RX rows are highlighted as `new`, tap global `Read`, and verify the unread count clears; then open a DM thread and verify its `Read` action clears only that thread.
13. For muted DM behavior when an inbound DM source is available, mute that contact, receive a DM, and verify the unread row is counted under `muted_dm_unread` rather than audible `dm_unread`.

## DM Store And Serial TX

For Phase 4 direct-message store validation:

Operator note: the other local MeshCore bot may be used as the controlled DM RF target for production validation. Use the targeted DM path when Public-channel RF should stay quiet.

1. Verify `contacts` contains a canonical `chat` contact with a full 64-hex `public_key`.
2. Run `messages dm clear`.
3. Run `mesh send dm <fingerprint> <text>`.
4. Verify `messages dm` contains a TX row with the contact fingerprint, alias, text, `direction="tx"`, `persisted=true`, and a nonzero `ack_hash`.
5. Verify `messages dm <fingerprint>` returns `filtered=true`, the same fingerprint, and only rows for that retained thread.
6. When more than one serial page is retained, verify `messages dm offset 8` and `messages dm <fingerprint> offset 8` report page metadata and return older retained rows in chronological order.
7. If the target contact exposes an explicit controlled-peer status receipt on an allowed, distinct local port, verify its status counters include fresh `rx_contact_total` movement. Use this targeted DM path instead of Public-channel RF when the operator asks to keep Public quiet.
8. If the peer emits MeshCore ACK/PATH returns, verify `messages dm` marks the TX row `acked=true` and `contacts` shows `out_path_known=true`.
9. Send a second DM to the same contact and verify `routes` records `kind="dm_text"`, `direction="tx"`, and `route="direct"`.
10. Reboot.
11. Verify `messages dm` and `messages dm <fingerprint>` retain the TX row and `health` reports `board_ready=true`, `ui_ready=true`, and increasing uptime.

Windows-alternative controlled-peer DM template (historical for the current Pi
route):

```powershell
$env:D1L_PORT = "COM12"
$env:MESH_PEER_PORT = "<explicit-allowed-peer-port>"
$env:MESH_PEER_STATUS_PATH = "<controlled-peer-status.json>"
python .\scripts\probe_d1l_dm.py --port $env:D1L_PORT --peer-status $env:MESH_PEER_STATUS_PATH --peer-port $env:MESH_PEER_PORT --out artifacts\hardware\com12\dm_probe_<full-commit>.json
```

Success requires schema 2, exact clean-commit metadata, `mode=hardware-dm-probe`, `physical_observed=true`, `dry_run=false`, `simulated=false`, `dm_rf_tx=true`, `public_rf_tx=false`, `public_rf_transmit=false`, `formats_sd=false`, no command beginning with `mesh send public`, `mesh send dm` returning `ok=true`, `messages dm <fingerprint>` and `packets search <token>` retaining the token, `routes trace <fingerprint>` matching the target, `health` ready, and the explicitly allowed controlled-peer status showing at least `rx_contact_total +1`.

Windows-alternative full controlled-peer RF template (historical for the
current Pi route):

```powershell
$env:D1L_PORT = "COM12"
python .\scripts\rf_full_acceptance_d1l.py --port $env:D1L_PORT --commit <full-commit> --token rf_accept_<short> --out artifacts\hardware\com12\rf_full_acceptance_<short>.json
```

Keep the runner attached only to the D1L serial port. When it prints the
controlled-peer inbound command, send that command through an independently
controlled compatible peer. The default qualification path needs no local peer
serial port: COM12 must itself retain the unique outbound and inbound tokens,
an acknowledged TX, ACK/PATH packet evidence, and the proven direct route. If a
local peer-status file is additionally used, pass `--peer-status` and an
explicit `--peer-port`; the runner rejects a missing, same-as-D1L, or forbidden
peer port. Success requires one complete newest schema-2
`rf_full_acceptance_*.json` hardware artifact with clean exact-commit metadata,
the real-hardware safety boundary, `identity_public_key_matches`,
`controlled_peer_observed`, `outbound_dm`, `inbound_dm`, `ack_path`,
`direct_route`, `health_ready`, and `no_public_commands` all true. The
`outbound_dm_controlled_peer` and `full_rf_dm_acceptance` release gates reject
schema-1, dry-run, simulated, source-inspection, predecessor, dirty-tree, and
partial receipts.

## Touch DM Compose

For Phase 4 touch direct-message compose validation:

1. Verify `contacts` contains at least one canonical `chat` contact with a full 64-hex `public_key`.
2. Open Network, tap the keyed chat-contact row, and verify Contact Detail exposes `Message`. Repeat with a repeater, room, sensor, or unknown role and verify no Message control is rendered.
3. Tap `Message`, type a short message on the compose keyboard, and tap `Send`.
4. Verify the toast reports the DM queued.
5. Verify `messages dm` contains a TX row for the same contact and message text with a nonzero `ack_hash`.
6. If physical touch cannot be observed in the current test session, run the backend precondition probe by sending the same target through `mesh send dm <fingerprint> <text>` and recording `contacts`, `messages dm`, and `health`.

For Phase 4 touch direct-message thread validation:

1. Verify `messages dm` contains at least one row for a contact with a full public key.
2. Open the Messages tab and tap the DM preview row.
3. Verify the full-height DM Thread opens with Back, the contact alias, fingerprint metadata, retained rows for the same fingerprint, and one sticky `Reply` action. Opening the thread marks it read automatically.
4. Tap `Reply`, type a short message, and tap `Send`.
5. Verify `messages dm` contains the new TX row for that fingerprint and `health` remains `board_ready=true` and `ui_ready=true`.
6. If a long hardware validation session reports `ESP_ERR_NVS_NOT_ENOUGH_SPACE`, stop persistence mutations and capture `storage status`, retained-store counters, `settings get`, and `health`. Treat the condition as a release-blocking durability failure; do not manually erase NVS, settings, identity, contacts, or current history to make the test pass. Current candidates must report `retained_nvs.partition="d1l_retained"`, `marker_ready=true`, `markers_complete=true`, `anchor_ready=true`, `sentinel_ready=true`, `external_init_required=false`, `ready=true`, `init_error="ESP_OK"`, and `migration_error="ESP_OK"`; retained writes go to that dedicated 124 KiB partition and v2 markers occupy both metadata slots. Verify blank first-use ordering as marker 1, NVS initialization, committed `d1l_ret_meta/anchor`, marker 2, scoped legacy migration, then the default-NVS sentinel. Do not use `nvs_flash_init_partition` as a read-only classifier: ESP-IDF initialization may erase or activate a corrupt page. For ambiguous nonblank bytes with neither marker nor sentinel ownership, require `external_init_required=true`, zero NVS-init calls, and zero retained-region erase attempts from firmware. Run `scripts/prepare_retained_nvs_upgrade_d1l.py` only with the exact running SHA and exact scope confirmation. Require exact table entries/MD5/artifact hash; for the failed pre-anchor incident also require the code-pinned manifest, exact historical receipt hashes, live MAC, audited prior MeshCore fingerprint, and exact complete raw-region SHA. Deliberately vary each field and prove refusal occurs before `erase_started`. On the accepted one-time path require `0x7E1000`/`0x1F000` bounds, staged/fsynced receipt state before and after erase, no reset between raw read/erase/verification, all-`0xFF` verification before hard reset, no default-NVS/meta/SD/RF mutation, and no retained raw backup. After that external step, firmware must observe blank first use and complete the ordered initialization. Marker- or sentinel-owned recovery must issue zero explicit retained-region erases, sentinel-only marker recovery must require the existing anchor, and the sole blank owned-state resume is marker 1 valid, marker 2 erased, and no default sentinel. Seed the published pre-anchor v1 markers and prove ownership upgrades without data erase: initialize, commit the anchor, migrate legacy keys, commit the sentinel, then rewrite both metadata slots as v2 with `markers_complete=true`. Simultaneous loss of both markers and the sentinel, including an anchor-only NVS, must remain preserved with `external_init_required=true`; firmware must not delete it automatically. On upgrade, known legacy retained keys are probed read-only and may be erased from default NVS only after their same-key dedicated copies commit. Identical duplicate copies may be reclaimed, but divergent copies are both preserved and make migration fail closed. No whole-default-NVS erase exists. Resume other recovery only with the audited external-initialization procedure or a separately authorized factory-reset/recovery test.
7. For Phase 5/Phase D connectivity validation, run smoke with `--persistence-test`. It must first snapshot a safely parseable node name, path-hash setting, Wi-Fi/BLE enable flags, and read-only Wi-Fi profile metadata; mutate only node name and path-hash; prove them across a route-flushed, nonce-changing reboot; then restore and verify those exact two original fields without issuing `settings reset`, `wifi off`, `ble off`, or changing the saved SSID/password-presence metadata. If the snapshot, initial mutation readback, or restoration cannot be proven, it must fail without another reboot. On the downloaded Actions artifact only, explicitly test `wifi scan` and `wifi connect` with a local 2.4 GHz network when hardware/network validation is in scope.
8. For Phase 7 diagnostics validation, run `crashlog clear`, `reboot`, then verify `crashlog` contains a new `SW` reset entry and `health` reports nonzero stack watermarks with `board_ready=true` and `ui_ready=true`.

## Heard Node Store

For Phase 4 heard-node validation:

1. Run `nodes clear`.
2. Wait for or trigger a signed MeshCore advert from a local node.
3. Verify `nodes` reports `active_capacity=64`, `capacity=64`, `sd_history_capacity=512`, `sort="last_heard"`, and at least one row with fingerprint, full 64-hex `public_key`, name or fingerprint fallback, `display_name`, `type`, production `role`, RSSI/SNR, path metadata, `favorite`, `keyed`, `reachable`, and `persisted=true`. NVS remains the compact fallback and retains the newest 16 heard-node rows until SD node history is implemented.
4. Reboot.
5. Verify `nodes` retains the row and its `public_key`.
6. Verify the host contracts cover `d1l_node_store_query()` filters for companions, repeaters, room servers, sensors, favorites, keyed-only, reachable-only, and sort modes for last heard, signal, name, role, and favorites.
7. Until the touch sort/filter sheet is complete, use serial `nodes`, `repeaters`, `roomservers`, `contacts set <fingerprint> favorite <0|1>`, and the UI simulator large-mesh view as the production proof that the query foundation and visual summaries agree.

## Contact Store

For Phase 4 contact-store validation:

1. Run `contacts clear`.
2. Verify `contacts` reports `count=0`.
3. Verify `nodes` contains a heard node with a 16-hex fingerprint.
4. Run `contacts add <fingerprint>`.
5. Verify `contacts add` reports `source="heard_node"`.
6. Verify `contacts` contains the promoted alias, full 64-hex `public_key`, heard name, type, RSSI/SNR, path metadata, `out_path_known`, `out_path_len`, and `persisted=true`.
7. Run `contacts rename <fingerprint> <alias>` and verify `contacts` shows the new alias with `persisted=true`.
8. Reboot.
9. Verify `contacts` retains the renamed row and its copied `public_key`.
10. If deleting a promoted contact is in scope for the test unit, run `contacts delete <fingerprint>` and verify `contacts` removes only the contact row while retained DM/message/route/packet history remains available.

For Phase 4 contact detail and favorite/mute validation:

1. Verify `contacts` contains at least one promoted contact.
2. Run `contacts set <fingerprint> favorite 1`.
3. Run `contacts set <fingerprint> mute 1`.
4. Verify `contacts` shows `favorite=true` and `muted=true`.
5. Reboot and verify both flags are still true.
6. Run `contacts set <fingerprint> favorite 0` and `contacts set <fingerprint> mute 0`.
7. Verify `contacts` shows both flags false.
8. For physical touch review, open Network and tap a canonical chat contact: Contact Detail exposes only `Back`, `Message`, and `Contact options`. A repeater, room, sensor, or unknown contact exposes no Message control. Open Contact Options and verify Route trace, Rename, favorite/mute, and Forget are reachable. A known canonical role with a valid key exposes Export QR; an unknown/malformed role or missing key shows a non-clickable Export unavailable row. Verify every actionable child Back returns to Contact Options. Enter Forget, then use Back/Cancel and confirm the contact still exists; perform the explicit destructive confirmation only when deletion is intentionally in scope.

For Phase 6 contact export validation:

1. Verify `contacts` contains at least one promoted contact with a full 64-hex `public_key` and canonical chat/repeater/room/sensor role; also retain or synthesize one unknown-role row for the fail-closed UI check.
2. Run `contacts export` and verify it returns `ok=true`, `format="meshcore://contact/add"`, and a list of entries whose `shareable` field reflects public-key availability.
3. Run `contacts export <fingerprint>` for the keyed contact.
4. Verify `meshcore_uri` starts with `meshcore://contact/add?`, includes URL-encoded `name`, the 64-hex `public_key`, and the correct numeric MeshCore `type`.
5. Open the known-role Contact Detail, tap `Contact options`, then `Export QR`; verify the Contact Export page shows a MeshCore QR plus URI metadata without crashing or exposing the background dock. Back must return to Contact Options. Open the unknown-role Contact Options and verify `Export unavailable` is non-clickable and no QR page opens.
6. If a MeshCore phone/client is available, scan the QR and verify it imports the same contact name/public key/type.

## Radio Settings

For Phase 6 radio settings validation:

1. Run `radio get` and verify the default Canada/USA profile reports 910.525 MHz, BW62.5, SF7, CR5, 20 dBm, RX boost enabled, TCXO `NONE`, `applied_to_radio`, `radio_apply_pending`, and `radio_apply_error`.
2. Run `radio set txpower 19`, then `radio get`, and verify `tx_power_dbm=19` with `persisted=true`, an unapplied radio profile, `radio_apply_pending=true`, and a non-empty `radio_apply_error` when the saved profile no longer matches live RF.
3. Run `radio set rxboost 0`, then `settings get`, and verify the nested `radio.rx_boost=false` field plus the radio apply status fields are present.
4. Run `radio set preset uscan`, then `radio get`, and verify the US/CAN defaults are restored before any RF regression.
5. Open Settings, tap `Radio`, change at least one staged value, tap `US/CAN`, tap `Save`, and verify the Radio Settings sheet stays readable and reports whether live RF matches the saved profile or RF apply remains pending.
6. Verify `health` remains `board_ready=true` and `ui_ready=true`.

## Route Store

For Phase 4 route-store validation:

1. Run `routes clear`.
2. Verify `routes` reports `count=0`.
3. Run `mesh send dm <fingerprint> route_store_test` against the controlled peer.
4. Wait for its DM/ACK response.
5. Verify `routes` contains DM TX and RX/ACK evidence with route name, direction, path hash bytes, hops, confidence, RSSI/SNR, and payload length. The response must expose `persistence.commit_count`, `coalesced_count`, `fail_count`, `dirty`, `sd_primary.backend_generation`, and `sd_primary.reconcile_pending`; while dirty, top-level `persisted` must be false rather than claiming the newest RAM observation is durable.
6. Poll `routes` without generating traffic. The first material dirty observation must not write before a full 5-second coalescing floor. After a clean commit, a same-route nonmaterial update must not write early merely because an older attempt completed; it must flush by the 30-second dirty deadline. Allow 35 seconds for polling jitter, then require `dirty=false`, `persisted=true`, and an increased commit count. Repeated hot observations inside the window must increase `coalesced_count` rather than synchronously rewriting storage. Explicit schema migration and a newly available SD reconciliation may run immediately, but a real failed attempt, including a controlled-reboot force flush, must still honor the 5-second retry floor and return the pending backend error without another write until eligible. If SD reads continue to fail, the dirty compact NVS snapshot must still advance while `sd_primary.reconcile_pending=true`, SD write count remains unchanged, and controlled reboot remains cancelled.
7. Pick a fresh route `seq` and run `routes detail <seq>`.
8. Verify the detail response matches the selected route row, including target, kind, route, direction, path metadata, signal metadata, and payload length.
9. Reboot. The controlled command must report `route_flush="ESP_OK"`; if the forced flush fails, it must cancel the reboot and preserve the dirty state for diagnosis. Host reboot/remount, retained-history, smoke-persistence, and release-audit evidence must reject a missing or non-`ESP_OK` route-flush field even when a changed boot nonce proves that some reboot occurred.
10. Verify `routes` retains the rows. With SD ready, the full newest 16 routes remain the primary snapshot and the newest 4 form the compact NVS fallback; without SD, those 4 fallback rows must load without erasing unrelated NVS state. Exercise a boot where the valid SD primary is temporarily unreadable or unavailable, then becomes ready: `reconcile_pending` must clear only after the full primary is read, same-epoch rows plus any live overlay are merged in original sequence order, and no 16-to-4 truncating write occurs. Toggle the retained backend false then true between two worker polls and replace the mocked primary in between; its monotonic generation must force reconciliation before the next SD write, preserving both the replacement primary and live overlay. A full-ring/low-index update case must leave the actual newest four overlays in compact NVS. Also seed an underfilled but newer compact fallback beside a readable same-epoch full primary: the SD snapshot must retain the full bounded union and NVS must be refilled with that union's newest four. For rollback freshness, a same-epoch legacy snapshot with a larger `total_written` must win. A normal migration keeps the legacy key, but an injected first `routes_v2` write returning `ESP_ERR_NVS_NOT_ENOUGH_SPACE` may erase only that obsolete legacy route-cache key and retry; erase/retry failure must remain dirty and block reboot. Correct-size SD and NVS blobs with an unterminated string, duplicate/out-of-range sequence, invalid count, non-printable field, or impossible path metadata must fail closed before any row is exposed and without a recovery erase. Fill all 16 durable rows, inject a volatile UI canary, then append one real route: the canary must change no durable metadata and the persisted set may lose only the legitimate oldest durable row. After an injected clear failure, repeated timed and forced flushes must perform zero backend writes until a later explicit clear succeeds.
Before physical review, inject a retained-backend generation change from the file-write hook after temp chunks are accepted but before replace-rename. The guarded write must return `ESP_ERR_INVALID_STATE`, perform no rename and no cleanup delete against the replacement card, preserve its primary, then reconcile that primary with the live overlay on the next forced flush. Run the same retained-store guard directly so Public/DM/packet split writes are covered by the common commit path.

11. For physical touch review, open the Packet tab, tap a route row, verify the route detail sheet opens with the same fields, and close it.

For Phase 6 retained route trace validation:

1. Verify `contacts` contains a promoted contact or use a known 16-hex fingerprint.
2. Run `routes trace <fingerprint>`.
3. Verify the response returns `cmd="routes trace"`, the requested `fingerprint`, `known_contact`, `contact_route`, `route_count`, `best_route`, `best_confidence`, and an `entries` array filtered to that target.
4. Verify `active_probe_supported=true` and `active_probe_command="routes probe <fingerprint>"`; plain `routes trace` still summarizes retained evidence and does not transmit RF.
5. Run `routes probe <fingerprint>` only when an opt-in DM RF trace is allowed. Verify the response has `cmd="routes probe"`, `queued=true`, a generated `trace_` token, `dm_rf_tx=true`, and `public_rf_tx=false`.
6. For physical touch review, open Contact Detail, tap `Contact options`, then `Route trace`; verify the page shows contact path, best evidence, retained route rows, and a `Ping` action. Back must return to Contact Options. Tap `Ping` only during RF-allowed validation and verify it queues the same DM-only active trace behavior without sending Public RF.

`routes probe` remains a DM token/PATH helper, not a canonical TRACE packet. The current real TRACE command is `routes trace contact <fingerprint>`; the runtime must re-resolve exactly one canonical full-key contact and a current-boot proven, nonexpired direct path, then derive the immutable loop without accepting operator path bytes. Contact-derived TRACE supports one- and two-byte hash widths with request flags 0 and 1. Generic TRACE wire helpers support flags 0 through 3, representing 1-, 2-, 4-, and 8-byte hashes; they do not make 4- or 8-byte contact routes available. A canonical three-byte contact route must fail closed because the pinned TRACE flags cannot represent that width. Run the contact command only during an explicitly authorized targeted-RF session, require `public_rf_tx=false`, then poll `routes trace status`. A qualifying receipt must require the expected request flag, the same tag, opaque correlation code, exact hash width, and immutable derived loop; it must classify source, in-flight, unsupported, malformed, duplicate, late, and timeout outcomes without treating them as success. `real_trace_contact_supported=true` reports this bounded software capability, while `hardware_verified=false`; controlled multi-hop official-peer RF, exact-candidate hardware acceptance, UI, and WP-04/WP-10 closure remain open.

## Packet Log

For Phase 6 packet-log validation:

1. Run `packets clear`.
2. Verify `packets` reports `count=0` and `persisted=true`.
3. Run `mesh send dm <fingerprint> packet_log_test` against the controlled peer.
4. Wait for its DM/ACK response.
5. Verify `packets` contains TX and RX rows with direction, kind, RSSI/SNR, path hash bytes, hops, payload length, and note text.
6. Pick a fresh packet `seq` and run `packets detail <seq>`.
7. Verify the detail response matches the selected packet row.
8. Reboot.
9. Verify `packets` retains the selected row. The RAM ring keeps 128 active rows; NVS persists the newest 8 fallback rows.
10. When `packet_log_backend="sd"`, verify `packets` reports `sd_history.enabled=true`, `sd_history.capacity=4096`, and `sd_history.failed_writes=0`; packet append must keep the NVS fallback fresh, commit the compact SD snapshot through `d1l_packet_log_flush()` or dirty/interval thresholds, and append fixed-size records into `stores/packet_log/segments/sNN.bin` for the 24h-target bounded SD history window.
11. Fill each retained Public, DM, route, and packet ring to capacity, snapshot its durable sequence/count/drop metadata and persisted blob, then inject the corresponding volatile UI canary. The canary must remain visible through the normal recent/query/thread/detail readers without changing durable metadata, evicting a retained row, writing NVS/SD, or appearing in a forced flush. The next real append must supersede the preview, reuse its borrowed sequence exactly once, and evict only the legitimate oldest durable row. Clear and re-init must remove every preview slot.
12. For physical touch review, open the Packet tab, tap a packet row, verify the packet detail sheet opens with the same fields, and close it.

## Optional SD-Card Data Storage

Important pending production feature:

The boot manager exposes `initial_ping_timeouts`, `bridge_response_seen`,
`auto_reset_pending`, and `auto_reset_attempted`. Before any valid DeskOS
response, three consecutive initial ping timeouts may queue exactly one
automatic reset-and-remount per ESP32 boot. The manager performs one final
bounded ping before claiming it. Any valid ping, status, or mount response
cancels a queued automatic pulse and permanently disables another for the boot.
Any physical reset starts a new timeout epoch and spends the boot's pre-response
reset budget. Manager and blocking serial remount sequences share one owner;
a blocking command racing an active sequence reports busy, while requests that
arrive during reset recovery are coalesced into the active reset and mount.
Acceptance must prove convergence without a serial recovery command, with
`public_rf_tx=false` and `formats_sd=false`.

Use `docs/RP2040_SD_BRIDGE_FLASH_D1L.md` for the RP2040 UF2 flash and post-flash proof sequence.

1. On the current D1L build, run `storage status` and verify `sd.interface="rp2040"`, `sd.direct_supported=false`, `sd.rp2040_protocol_supported` reflects whether the RP2040 bridge answered `DESKOS_SD_STATUS`, `sd.file_ops`, `sd.file_line_max`, `sd.file_chunk_max`, `sd.path_max`, and `sd.atomic_rename` expose the exact file-operation gate, `data_backend` is either `nvs` or `mixed`, `setup_action` is machine-readable, retained store backends keep settings, identity, contacts, read-state, and crashlog on onboard/fallback storage, and `export_backend`/`stores.exports` report either `serial` or `sd_diagnostic_exports_ready`.
2. Run `storage setup` and verify it reports `will_format=false`, `format_requested=false`, `format_performed=false`, `policy="no_device_format"`, and `fallback="nvs"` without modifying onboard data.
3. Verify no serial command, UI action, RP2040 request, script flag, or hardware test path can format an SD card.
4. Verify Settings opens a full-height read-only Storage root with only Card status and Data locations destinations. Card status must use plain-language media guidance, Data locations must scroll within its bounded panel, Back must return one level at a time, and the fixed footer must state that DeskOS never formats cards. No page may render a raw setup-action slug or attach mount, remount, delete, format, or other mutating callback.
5. Boot with no card and verify firmware continues with onboard storage defaults.
6. After the RP2040 SD bridge firmware is flashed through a documented RP2040 path, boot with a valid DeskOS-formatted card and verify serial/UI status reports card/root readiness. If boot prepare reaches the ready file gate before store init, verify `message_store_backend="sd"`, `dm_store_backend="sd"`, `route_store_backend="sd"`, `packet_log_backend="sd"`, `data_backend="mixed"`, and `setup_action="retained_history_sd_enabled"` while settings/identity/contacts/read-state/crashlog remain onboard-backed. `export_backend` may report `sd_diagnostic_exports_ready`, and map tiles may report `sd_map_tiles_ready` when the map-tile file-operation gate is ready. If boot prepare times out or remains `mount_pending`, fallback must remain NVS while `storage status.manager.state` reports the retry state; run `storage remount` or the acceptance runner to prove later convergence without ESP32 reboot.
7. Run `python .\scripts\sd_boot_prepare_acceptance_d1l.py --port COM12 --scenario correct-structure`, `python .\scripts\sd_boot_prepare_acceptance_d1l.py --port COM12 --scenario missing-structure`, and `python .\scripts\sd_boot_prepare_acceptance_d1l.py --port COM12 --scenario existing-data` to prove correct DeskOS cards, valid filesystems with missing `/deskos`, and existing DeskOS data are preserved/prepared without formatting. Only an explicit manager-busy remount receipt (`ESP_ERR_INVALID_STATE` with `manager.running=true`) may trigger bounded fresh-status polling and exactly one retry. A cached `READY_SD` sample from the same `manager.attempt` is insufficient: require a later attempt when that counter is present, or otherwise observe a transitional/non-running manager state before a fresh clean `READY_SD` sample. The final retry must report `ok=true` and `retained_worker_quiesce_acquired=true` before any file canary, setup, or health command. The autonomous `--refresh-rp2040-smoke --dry-run` output is planning-only and cannot capture `rp2040-unavailable`; actual `no-card`, `unformatted`, and bridge-unavailable states require the narrow physical acceptance paths.
8. With a non-FAT32 or unmountable card inserted, run `python .\scripts\sd_boot_prepare_acceptance_d1l.py --port COM12 --scenario unformatted` and verify it records `formats_sd=false`, does not send a setup-confirm command, keeps NVS fallback active, and tells the user to prepare FAT32 on a computer.
9. With the RP2040 bridge file protocol flashed, run `storage filecanary` or `python .\scripts\sd_file_canary_d1l.py --port COM12` to perform the serial-only file-operation canary under `/deskos`: temp write, read-back compare, `rename replace=1`, stat, final read, delete, and deleted-stat verification. Then run `storage export-canary <token>` or `python .\scripts\sd_export_canary_d1l.py --port COM12 --token export1` to prove the diagnostic export path writes `exports/diagnostics/export-canary-<token>.json` by temp write/read plus atomic rename and leaves the final file present. Then run `storage export-diagnostics <token>` or `python .\scripts\sd_diagnostic_export_d1l.py --port COM12 --token diag1` to prove a chunked diagnostic export JSON bundle writes to `exports/diagnostics/diagnostic-export-<token>.json`, verifies temp and final readback, and reports map tile cache readiness without bundling actual tiles. Then run `storage export-data <token>` or `python .\scripts\sd_data_export_d1l.py --port COM12 --token data1` to prove a sampled user-data JSON bundle writes to `exports/data/data-export-<token>.json`, verifies temp and final readback, and reports `private_identity_exported=false`. Then run `storage map-tile-canary <token>` or `python .\scripts\sd_map_tile_canary_d1l.py --port COM12 --token map1` to prove the map-tile cache path writes `map/tiles/z12/x1/y2-<token>.tile` through temp write/read plus atomic rename and leaves the final synthetic tile present. Then run `python .\scripts\sd_retained_history_acceptance_d1l.py --port COM12` to append synthetic retained Public, DM, route, and packet rows through `storage retained-canary <token>`. Require the command's four positive sequence IDs, four `sd` backends, `storage_manager_quiesced=true`, `retained_worker_quiesced=true`, and explicit `public_rf_tx=false`/`formats_sd=false`. Before reboot, require a bounded full persistence poll to prove Public, DM, route, packet, and storage snapshots are all clean, with zero retained SD/NVS failure counters, `ESP_OK` errors, no degradation latches, and `manager.state=READY_SD`; a transient `STATUS` / `wait_for_retained_worker` sample may be retried but is never itself passing evidence. Poll exhaustion, timeout, or an unexpected boot must report `pre_reboot_gate_passed=false`, `reboot_attempted=false`, and a precise `reboot_skipped_reason`; it must not issue `reboot`. A failed canary may still collect its bounded read-only token/fingerprint readbacks, status, and health for partial-write diagnosis under the same fail-closed boundary. `sd_reboot_remount_acceptance_d1l.py` must enforce this boundary and additionally require successful pre-remount and map-tile canary receipts. Before and after the nonce-changing `route_flush="ESP_OK"` reboot, each readback must contain a non-empty row with its exact sequence and canary fields, with consistent page/total/route counters. Echoed search or fingerprint fields are not retention evidence. Follow with `python .\scripts\soak_d1l.py --port COM12 --duration-sec 300 --sample-interval-sec 60 --sample-storage --sd-file-canary` so the file canary is repeated during a passive stability window. Do not send Public RF for this validation.
10. Boot with a present but unrelated existing-data card and verify firmware offers NVS fallback or clear backup/reformat-on-computer guidance without silently wiping data.
11. Confirm incidental boot/touch events never write, delete, or format SD data.
12. Use `tools/rp2040_sd_protocol.py` to verify the reference ping, status, diagnostic, and file-operation line grammar for `no-card`, `ready`, and `needs-fat32` scenarios before implementing retained-store migration. Use `python .\tools\rp2040_sd_protocol.py --request DESKOS_SD_PING` to print the no-SD-touch bridge ping, `python .\tools\rp2040_sd_protocol.py --request DESKOS_SD_DIAG --scenario no-card` to print the bridge diagnostic line, `python .\tools\rp2040_sd_protocol.py --scenario ready --file-canary-transcript` to print the deterministic host transcript that mirrors `storage filecanary`, `python .\tools\rp2040_sd_protocol.py --scenario ready --export-canary-transcript --token export1` to print the export-canary transcript, `python .\tools\rp2040_sd_protocol.py --scenario ready --diagnostic-export-transcript --token diag1` to print the chunked diagnostic-export transcript, and `python .\tools\rp2040_sd_protocol.py --scenario ready --map-tile-canary-transcript --token map1` to print the map tile cache transcript.
13. With no card, protocol timeout, `file_ops=0`, `atomic_rename=0`, or smaller-than-required line/path/chunk limits, verify packet-log persistence remains on NVS and survives reboot with the existing `d1l_packets`/`ring` fallback data location.
14. When configured, verify retained Public history writes `stores/messages/public/public.tmp` then commits `stores/messages/public/public.bin`, DM history writes `stores/messages/dm/threads.tmp` then commits `stores/messages/dm/threads.bin`, route history writes `stores/routes/routes.tmp` then commits `stores/routes/routes.bin`, and packet compact history writes `stores/packet_log/ring.tmp` then commits `stores/packet_log/ring.bin`; all compact blobs must use `rename replace=1`, keep the NVS mirror, and fall back to NVS on SD absence, timeout, or corrupt blobs. Packet history also writes a bounded 64 x 64 record SD journal under `stores/packet_log/segments/sNN.bin`; records use sequence-derived fixed offsets, segment-cycle starts may truncate the DeskOS-owned segment, exact matching records are idempotent retries, and the canonical unwritten-slot response is `len=0 eof=1 data= crc=00000000`. `packets clear` may delete those DeskOS-owned segment files without formatting the card.
15. Run `storage map-tile-canary <token>` with Wi-Fi off and verify the synthetic tile uses temp write/read plus atomic rename, reports `public_rf_tx=false`, `formats_sd=false`, and performs no network request. This proves storage only; it is not live-map acceptance.
16. For one quick live Map acceptance, set a location, connect Wi-Fi, then open the actual Map. Verify it opens at zoom 10 with `(c) OpenStreetMap contributors` visible. Drag with one finger and prove the image previews during motion but no tile plan/request begins until release; then verify at most nine tiles for that one newly visible view. Exercise `-` and `+` through zooms 8 and 14, confirm the controls stop at those bounds, and verify each tap creates at most one current-view 3x3 plan at one zoom per visible generation. Tap `Center` and verify the saved coordinates are restored. Leave Map during an unfinished batch and verify remaining requests stop. After one view is complete, go Home and return to Map; require the same frame generation/revision and unchanged attempted/cache-hit/download/network counters, plus no SD read operation in the trace, so the retained frame appears without network or SD reread. Power-cycle, reopen that view, and verify the SD tile cache may be reread but cached tiles are not downloaded again. Then run `python .\scripts\scroll_probe_d1l.py --port <D1L_PORT> --screens map,map_options,map_location,map_cache`; verify its successful before/after `map tiles status` rows have equal `network_requests`, and verify `network_tx=false`, `map_network_requests=false`, `background_download=false`, `area_download=false`, `visible_tile_limit=9`, and `zoom_batch_limit=1`. Never run a background, multi-zoom-prefetch, off-screen arbitrary-coordinate batch, or area download.
17. Verify settings, identity, and minimum boot-critical state remain available from onboard storage if the card is removed.
18. Verify the `rp2040-sd-bridge-firmware` artifact checksum manifest before any RP2040 hardware flash attempt.
18. Before copying the RP2040 UF2, run `python .\scripts\rp2040_sd_bridge_preflight_d1l.py --port COM12 --artifact-dir artifacts\github\<run-id>\rp2040-sd-bridge-firmware --out artifacts\rp2040-preflight\d1l-rp2040-sd-bridge-preflight-COM12.json`. If it reports `rp2040_protocol_pending` or `sd_card_not_present_diag_pending` and no UF2 volume, put the RP2040 into UF2/BOOTSEL mode. If it reports `sd_bridge_ready`, proceed directly to the SD file/export canaries.

## Mesh Visibility

For Phase 6 signal/room-server/repeater validation:

1. Run `signal` and verify `sample_count` is nonzero after live RX, `latest.rssi_dbm` is nonzero, and RSSI/SNR values reflect recent packet, route, or heard-node evidence.
2. Run `roomservers` and verify `total_known` and `entries` reflect signed heard-node adverts whose stored role is `room`.
3. Run `repeaters` and verify entries are inferred only from nonzero path-hop route or heard-node evidence; Public route rows should not by themselves become repeater candidates.
4. Run `mesh send dm <fingerprint> mesh_visibility_test`, wait for the controlled peer response, and verify D1L packet count increases. If an explicitly allowed controlled-peer status receipt is used, also require fresh DM/contact movement there.
5. Verify `health` remains `board_ready=true`, `ui_ready=true`, and reports nonzero task stack watermarks after the probe.
6. For host proof, verify the simulator emits `mesh_roles_sheet`, `mesh_rooms_page`, and `mesh_repeaters_page`; each button target is at least 44x44, source rows stay contained in the list panel, and no Mesh Roles action is RF-capable or destructive.
7. On exact Actions-built COM12 firmware with retained packet/route data still populated, capture the three read-only aliases `ui scroll-probe mesh_roles`, `ui scroll-probe mesh_rooms`, and `ui scroll-probe mesh_repeaters`. The probe timeout must accommodate rebuilding a full Packet view without timing out, static nested pages must synchronously flush the requested page before reporting completion, and a timed-out request must retain its in-flight owner until its late completion is published so no later request can consume stale evidence. Verify capture metadata reports matching firmware/host CRCs, `public_rf_tx=false`, and `formats_sd=false`.
8. For physical touch review, open Packets and tap `Mesh Roles`. Verify the root shows only the Rooms and Repeaters categories; each opens its own bounded vertical list. Child Back must return to Mesh Roles, root Back must return to Packets, rows must stay inside the list while scrolling, and tapping a row must perform no action.

PR #60 / source `0b138be` is the closed Mesh Roles baseline: push Actions `29068006554` and PR Actions `29068007961` passed; COM12 Mesh Roles, Rooms, and Repeaters CRCs were `63DE54FB`, `FD538D71`, and `5C41EE08`, with matching host CRCs, passing simulator diffs, and a clean three-round probe without Public RF or formatting. Do not rerun it for the Storage slice, and do not claim Storage hardware proof until its own exact Actions/COM12 captures pass.
