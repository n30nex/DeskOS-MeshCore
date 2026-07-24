# Fast D1L Release Workflow

Use this loop until the release gate is green. The rule is simple: one issue, one
small PR, one targeted hardware proof. The goal is to close release blockers
without spending bench time re-proving unrelated UI, SD, RF, or soak surfaces on
every PR.

Do not rerun RP2040/SD work unless the issue changes SD/RP2040 code or SD
evidence is the selected blocker. Do not run the bundled UI validator just
because an issue is "UI"; run the one COM12 proof that matches the issue's
acceptance criteria.

## Default Cycle

1. Start from current `main`.
2. Pick one open P0 issue from GitHub.
3. Make the smallest code/docs/test change that can close that issue.
4. Run focused host tests first.
5. Run `python -m pytest tests -q` only after the focused set passes.
6. Push and use the default `d1l-ci` path. It should skip RP2040/SD work unless
   SD/RP2040 files changed.
7. Download the ESP32 Actions artifacts and verify checksums.
8. Run exactly one targeted COM12 hardware proof for the selected issue.
9. Update the issue and PR with artifact paths.
10. Merge the PR if checks and targeted hardware proof pass.

## Validation Tiers

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
python .\scripts\verify_checksums.py artifacts\github\<run-id>-<sha>\d1l-release-package
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

## Local Progress Dashboard

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

## Stop Conditions

Stop the cycle only when one of these is true:

- The selected issue is merged and the issue is closed.
- Hardware evidence proves the fix is wrong and a new issue/comment is created.
- The device/port route is unsafe or unavailable.

Do not stop because broad release remains blocked after a narrow issue is
closed. Move to the next P0.

## Final Production Sweep

When the issue-sized P0 list is empty, run the full production sweep: release
gate audit, current COM12 smoke/UI evidence, required RF/DM proof, remaining SD
matrix evidence, manual photos/review, and the long soak. This sweep is what
turns "all blockers closed" into "safe to tag"; it is not the default proof for
every small PR.
