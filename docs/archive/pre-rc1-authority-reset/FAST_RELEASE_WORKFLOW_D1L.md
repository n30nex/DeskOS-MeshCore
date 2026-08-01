HISTORICAL RECORD — DO NOT EXECUTE

This document predates the RC1 authority reset. It is retained only for
provenance. It cannot create work, tests, evidence requirements, or release
gates. See `docs/RC1_SCOPE.md` and `docs/ROADMAP.md`.

# Fast D1L Release Workflow

This is the authoritative fast path for DeskOS 1.0 / RC1. The candidate is
`D1L_RELEASE_PROFILE=core_1_0` with
`D1L_SD_HISTORY_MODE=conditional`.

During implementation, run only the focused checks needed by the changed
slice. Do not repeat unrelated UI, SD, RF or edge-case campaigns. No soak is
part of RC1.

After the final change merges, stop using issue-sized examples and follow the
[authoritative RC1 release execution](RC1_RELEASE_EXECUTION_D1L.md) exactly.
It owns the exact-main Actions capture, one non-erasing retained-state flash,
bounded four-source gate, final audit, `v1.0.0` tag and release assets.

## Current cycle

1. Start from current `main` and make the smallest change that closes the
   selected RC1 issue.
2. Run focused host/source checks for that change plus `git diff --check`.
3. Push one immutable candidate and require its exact-SHA `d1l-ci` run.
4. Download the exact `core_1_0`/`conditional` package and verify checksums,
   inventory, provenance, SBOM, Actions identity and profile.
5. On Pi 5 host `neopi5`, select only
   `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0` after verifying
   `VID:PID 1a86:7523`. Never enumerate or probe another Pi serial device.
6. Flash the verified Actions artifact non-erasing. Do not use a local build,
   raw `/dev/ttyUSB*` name or stale Windows COM assignment.
7. Run only the hardware proof that matches the changed slice. For the final
   candidate, use the reduced bounded gate covering the exact non-erasing
   retained-state flash, boot advert and one operator-authorized Public send,
   DM/ACK, contact PATH, repeater Ping, repeater login/authenticated query,
   and authorized Map download/offline revisit. These are exactly four fresh
   evidence sources: flash, RF, protocol and Map. Do not rerun the completed
   UI-navigation, Wi-Fi-reconnect, SD write/reboot/remount, or prepared-card
   remove/reinsert campaigns. Carry the prior operator-observed SD cycle
   forward as context only; do not claim a fresh SD receipt or outcome.
8. Confirm SD-primary behavior, built-in OSM visible-only behavior, provider
   authorization, and prefetch pause while the interactive Map is open.
9. Record artifact paths and results against the issue/PR. Merge only after the
   selected focused proof passes.

## Current safety limits

- Firmware and validation never format or repair SD. Use a prepared
  32GB-or-larger FAT32 card.
- Missing/unusable SD must show live-only RF chat restrictions and must not
  redirect retained history into default NVS.
- Background/offline Map download requires connected Wi-Fi, configured
  location, ready SD and an HTTPS provider manifest explicitly authorizing
  offline storage and background prefetch.
- Automated validation transmits on the default Public channel only for the
  single tokenized final gate when the operator explicitly supplies
  `--authorize-public-tx`; every other automated path remains Public-silent.
- Use only a narrowly authorized controlled peer for DM/RF/Admin proof.
- BLE companion transport, QR sharing and signed OTA/update/recovery product
  workflows are deferred to 1.5 / RC2.

## Historical validation-tier examples

Everything below this heading predates the current Pi/by-id RC1 flow. Its
COM12/COM16 examples, Full Feature assumptions, broad test suites, soak
instructions and old SD/NVS policy are retained only to interpret historical
receipts. Do not execute them as current release instructions.

### ESP32/UI Issue

Use this for UI, compose, docs, simulator, serial command, and ESP32 app fixes.
This is the normal fast path for most remaining P0s.

```powershell
python -m pytest tests\test_ci_workflow_contract.py tests\test_release_gate_audit_d1l.py -q
python -m pytest tests -q
gh workflow run d1l-ci.yml --ref <branch> -f include_sd_bridge=false
gh run watch <run-id> --exit-status
gh run download <run-id> --dir artifacts\github\<run-id>-<sha>
python .\scripts\verify_checksums.py artifacts\github\<run-id>-<sha>\d1l-firmware-artifacts
python .\scripts\verify_checksums.py artifacts\github\<run-id>-<sha>\d1l-release-package\d1l-release-<sha>
```

Then run one issue proof:

| Selected issue | COM12 proof |
|---|---|
| Split-page or stale-column redraw corruption | `python .\scripts\ui_corruption_probe_d1l.py --port COM12 --rounds 20 --clear-crashlog-before-start --out artifacts\hardware\com12\ui_corruption_probe-<sha>-COM12.json` |
| Hardware pixel capture or SiguredOS Home proof | `python .\tools\ui_simulator.py --view home --out artifacts\ui-sim-reference\<sha>` then `python .\scripts\ui_capture_d1l.py --port COM12 --prep-command "ui tab home" --reference-png artifacts\ui-sim-reference\<sha>\home.png --reference-view home --out artifacts\hardware\com12\ui_pixel_capture-<sha>-COM12.json` |
| Compose/input keyboard geometry | `python .\scripts\ui_compose_keyboard_capture_d1l.py --port COM12 --targets all --out artifacts\hardware\com12\ui_compose_keyboard_capture-<sha>-COM12.json` |
| One scroll/layout surface | `python .\scripts\scroll_probe_d1l.py --port COM12 --screens <screen-or-small-list> --manual-touch --clear-crashlog-before-start --out artifacts\hardware\com12\scroll_probe-<sha>-COM12.json`; omit the Clear flag for `map|map_options|map_location|map_cache`. These probes arm Map network suppression before navigation, sample `map tiles status.network_requests` before and after Map automation, and must prove the counter stayed equal while reporting `map_network_requests=false`. |
| Docs, tests, host simulator, or non-hardware plumbing | No COM12 cycle unless the GitHub issue explicitly names hardware acceptance. Use Actions plus host artifacts. |

Expected proof for ESP32/UI work:

- `rp2040_uf2_flash=false`
- `sd_suite_enabled=false`
- no COM16/RP2040 flash/test path
- the artifact directly matches the selected issue
- `public_rf_tx=false` and `formats_sd=false`

The legacy bundled autonomous validator is planning-only until its direct
flash and mutation paths are rebound to the exact full D1L key and stable
target. It may still be inspected with `--dry-run`, but it is not a hardware
or release path:

```powershell
python .\scripts\autonomous_hardware_validate_d1l.py --github-run-id <run-id> --github-run-dir artifacts\github\<run-id>-<sha> --commit <sha> --skip-sd-suite --include-ui-probes --dry-run
```

Use the narrow exact-candidate flash, smoke, UI, reboot, RF, and soak runners
for hardware evidence.

### SD/RP2040 Issue

Use only when SD/RP2040 code or SD physical evidence is the selected issue.

```powershell
gh workflow run d1l-ci.yml --ref <branch> -f include_sd_bridge=true
```

Then run the SD-specific validator or guided SD workflow named by the issue.
Keep `formats_sd=false`; users prepare FAT32 cards on a computer.

### RF/DM Issue

Use a targeted RF/DM proof and keep ports explicit. Do not mix this with SD or
UI refactors in the same PR.

### Supported-SDK Migration (issue #63)

This release-blocking migration is intentionally broader than a normal
issue-sized UI proof. The `supported_sdk_baseline` check covers the workflow
selection of `espressif/idf:v5.5.4` and the committed lock target only; the
version tag is not a content-immutable image identity, the check does not prove
lock provenance, and neither is hardware qualification.

1. Run host policy tests and the complete host suite.
2. Let the version-pinned Actions environment generate `dependencies.lock`.
   Archive and review that exact output and diff; do not hand-edit its generated
   hash and do not use a local firmware build to regenerate it.
3. Commit the generated lock, rerun Actions, and require a clean lock plus
   passing firmware/package/checksum jobs. Retain the run/image/lock/artifact
   metadata together.
4. Flash that exact verified artifact to COM12. Require the serial `version`
   response to contain `"idf":"v5.5.4"`, then run the issue #63 board,
   display/touch, Wi-Fi, RF, RP2040/SD, Map, health, reboot, and post-power-cycle
   checks. Use COM16 only for explicitly required RP2040 proof.
5. Refresh the relevant commit-matched release evidence and keep the release
   fail-closed until every P0 gate is green.

### Historical local progress dashboard

Run this in a separate terminal while Codex works:

```powershell
python .\scripts\release_progress_dashboard.py --open
```

The dashboard is read-only. It reads the newest `artifacts\release-gate` and
`artifacts\hardware\d1l-autonomous-hardware-validation-*.json` files, then shows
overall progress, P0 progress, category progress, and the open P0 evidence
gates. It does not open serial ports, flash firmware, run Actions, or call
GitHub.

JSON snapshot:

```powershell
python .\scripts\release_progress_dashboard.py --once-json
```

### Historical stop conditions

Stop the cycle only when one of these is true:

- The selected issue is merged and the issue is closed.
- Hardware evidence proves the fix is wrong and a new issue/comment is created.
- The device/port route is unsafe or unavailable.

Do not stop because broad release remains blocked after a narrow issue is
closed. Move to the next P0.

### Historical final production sweep

When the issue-sized P0 list is empty, run the full production sweep: release
gate audit, current COM12 smoke/UI evidence, required RF/DM proof, remaining SD
matrix evidence, manual photos/review, and the long soak. This sweep is what
turns "all blockers closed" into "safe to tag"; it is not the default proof for
every small PR.
