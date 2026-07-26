# MeshCore DeskOS D1L Developer Guide

> **SUPERSEDED DEVELOPMENT/VALIDATION PROCEDURES.**
> The commands below preserve earlier Full Feature, Windows-COM and soak
> workflows; they are not current RC1 release instructions. Current authority
> is the [project README](../README.md), [RC1 user guide](USER_GUIDE_D1L.md),
> [RC1 test plan](TEST_PLAN_D1L.md), and
> [SD-card guide](D1L_SD_CARD_GUIDED_INSTALL.md). Use only the Pi 5 stable
> by-id D1L route and the exact downloaded GitHub Actions artifact described
> there.

## Repo Shape

- `main/` contains the ESP-IDF firmware app.
- `main/ui/` contains the LVGL touch shell.
- `main/mesh/` contains MeshCore service/store helpers.
- `main/comms/` contains USB console and connectivity status plumbing.
- `main/diagnostics/` contains health and crash/reset telemetry.
- `scripts/` contains host-check, flash, smoke, soak, backup, checksum, and release-package tooling.
- `firmware/rp2040_sd_bridge/` contains the Arduino RP2040 SD bridge target. It is compiled by GitHub Actions.
- `tests/` contains host contract tests.
- `docs/` contains the active README/index, user/developer guides, release checklist, known limitations, test plan, SD runbooks, protocol docs, screenshots, and attribution notes.

## Host Checks

```powershell
python -m pytest -q
python .\tools\ui_simulator.py --out artifacts\ui-sim
python .\tools\ui_simulator.py --scenario large-mesh --out artifacts\ui-sim-large
python .\tools\ui_simulator.py --scenario storage-states --out artifacts\ui-sim-storage
python .\scripts\smoke_d1l.py --dry-run
python .\scripts\ui_corruption_probe_d1l.py --dry-run --rounds 20
python .\scripts\ui_capture_d1l.py --dry-run
python .\scripts\scroll_probe_d1l.py --dry-run --screens home,public_messages,dm_thread,nodes,packets,settings,storage,storage_card,storage_data,wifi,map,map_options,map_location,map_cache
python .\scripts\soak_d1l.py --dry-run --duration-sec 60 --sample-interval-sec 15 --active-dm-fingerprint 0123456789ABCDEF --active-dm-text test
python .\scripts\sd_boot_prepare_acceptance_d1l.py --dry-run --scenario all
```

The final Full Feature idle soak runs on `neopi5` from the clean exact Actions
candidate checkout and uses the stable by-id target:

```bash
python ./scripts/soak_d1l.py \
  --port /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
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

Run conditional-SD canaries separately. The 12-hour idle/listening gate is
RF-silent and intentionally does not repeat storage writes for its duration.

## Firmware Build

Do not build firmware on the Windows host. Use GitHub Actions for ESP32
binaries. RP2040 SD bridge binaries are opt-in for bridge/SD work only:

```powershell
gh workflow run d1l-ci.yml --ref <branch-or-main> -f include_sd_bridge=false
gh run watch
gh run download <run-id> -D artifacts\github\<run-id>
```

For a bridge/SD release artifact refresh, run the same workflow with
`-f include_sd_bridge=true`.

The local `scripts/build_d1l.ps1` path is host-only and rejects `-RequireFirmware`.

## Release Package

After `build/` exists:

```powershell
python .\scripts\package_release_d1l.py --build-dir build --out-dir artifacts\release
```

The package includes:

- `firmware/bootloader.bin`
- `firmware/partition-table.bin`
- `firmware/meshcore_deskos_d1l.bin`
- `firmware/flasher_args.json`
- `update/meshcore_deskos_d1l-app.bin`
- `full-flash/meshcore_deskos_d1l-full-8mb.bin`
- `manifest.json`
- `SHA256SUMS.txt`
- `flash_project.ps1`
- `flash_project.sh`
- `flash_full_8mb.ps1`

## Hardware Validation

### Current Pi 5 route

The release-closing D1L is attached to the Raspberry Pi 5 host `neopi5`.
Connect as the unprivileged, key-only account `siguidev`; no password belongs
in this repository, a command line, an environment file, or an evidence
receipt. The only accepted current D1L selector is:

```text
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
```

Require USB VID:PID `1A86:7523`, a readable and writable symlink, and a
resolved device before opening it. Its current `/dev/ttyUSB2` resolution is
observational only and may change after a move or reboot.

Run repository hardware scripts on `neopi5` with the stable selector:

```bash
export D1L_PORT='/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
test -L "$D1L_PORT" && test -r "$D1L_PORT" && test -w "$D1L_PORT"
D1L_DEVICE_PROPERTIES="$(udevadm info --query=property --name="$D1L_PORT")"
grep -qx 'ID_VENDOR_ID=1a86' <<<"$D1L_DEVICE_PROPERTIES"
grep -qx 'ID_MODEL_ID=7523' <<<"$D1L_DEVICE_PROPERTIES"
python ./scripts/smoke_d1l.py --port "$D1L_PORT" --manual-touch
python ./scripts/ui_capture_d1l.py --port "$D1L_PORT" --out artifacts/hardware/neopi5/ui_pixel_capture-by-id.json
```

Use the exact candidate package's `flash_project.sh` for the non-erasing
release flash; do not build firmware locally or substitute a repository build
directory. `COM12` remains the valid Windows alternative if the D1L is moved
back to that host:

```powershell
$env:D1L_PORT = "COM12"
.\scripts\flash_d1l.ps1 -Port $env:D1L_PORT
python .\scripts\smoke_d1l.py --port $env:D1L_PORT --manual-touch
```

`COM16` is used only for separately authorized RP2040 USB smoke/UF2
maintenance and is never the Core D1L app/console/flash target. Never open
`COM8`, `COM11`, or `COM29` as the D1L target. Historical COM12 receipts remain
valid only for the exact candidates they name; they do not qualify the Pi-hosted
replacement candidate.

A controlled MeshCore peer may be used for production RF/DM validation only
after `siguidev` has narrowly scoped, verified access to the exact peer status
and control resources. That access is not complete merely because SSH and D1L
serial access work. Keep the peer transport explicit, prefer the targeted DM
probe when Public-channel RF should stay quiet, and do not use a peer serial
port as the D1L target.

Moving or discovering the device does not close release. Exact-SHA package and
flash identity, UI and manual review, reboot/retained-state proof,
protocol-time migration, controlled RF/DM, active and idle soak, installation
review, and the final Core audit remain fail-closed.

Do not format SD cards from DeskOS firmware, RP2040 firmware, serial commands, UI, or scripts. Production validation assumes users provide FAT32 cards prepared on a computer; the current validation device has a fresh FAT32 32 GB card installed. DeskOS may create the `/deskos` folders/manifests on a mounted FAT32 card and otherwise falls back to NVS.

The 8 MB layout preserves the default 24 KiB `nvs` partition for settings,
identity, Wi-Fi, contacts, nodes, read state, and crash state. Public, DM,
route, and packet fallback blobs use the separate 124 KiB `d1l_retained`
partition at `0x7E1000`; its 4 KiB metadata sector at `0x7E0000` holds two
versioned marker copies. The dedicated partition itself stores a versioned
`d1l_ret_meta/anchor`, and a final completion claim is stored in default NVS as
the `d1l_ret_meta/initialized` sentinel. The factory app remains at `0x10000`
and ends before the metadata sector. Upgrade reads copy a scoped legacy
retained key from default NVS only after the dedicated write commits, then
erase only that old key; the completion sentinel is committed after all known
legacy-key migration succeeds.

Marker format v2 is the release format. On blank first use, firmware writes
metadata marker 1, initializes NVS, writes and commits the dedicated anchor,
and only then writes metadata marker 2. Scoped legacy migration follows, and
the default-NVS completion sentinel is committed last. The anchor makes a
genuinely initialized user-empty store physically nonblank, while marker 2
proves that the anchor-commit point was crossed. The only blank owned-state
resume is the exact pre-initialization power-loss state with marker 1 valid,
marker 2 erased, and no default sentinel.

`nvs_flash_init_partition` is not a read-only probe. ESP-IDF initialization may
erase or activate a corrupt page, so firmware never uses it to classify a
nonblank region that has neither a valid current/future metadata marker nor a
valid default sentinel. It performs zero retained-region erases, returns
fail-closed status with `external_init_required=true`, and leaves the ambiguous
bytes untouched. The installer or hardware procedure must first verify the
supported predecessor partition/layout provenance, then perform a separately
audited erase scoped strictly to `d1l_retained` at `0x7E1000` for `0x1F000`
bytes. Firmware then verifies blank first use and executes marker 1 -> NVS init
-> anchor commit -> marker 2 -> legacy migration -> sentinel.
Use `scripts/prepare_retained_nvs_upgrade_d1l.py` for the external step. It
requires an exact running SHA from the audited predecessor allowlist, validates
the partition table's MD5 record, exact entries, and exact Actions artifact
hash, then requires the exact erase-scope confirmation. The one known failed
pre-anchor candidate is incident-specific: live failure-shaped status alone is
never authorization. Its committed evidence manifest is hash-pinned in the
tool and binds the exact flash and first-boot receipt hashes, COM port, ESP32-S3
MAC, the previously captured MeshCore identity fingerprint, and the complete pre-erase
`d1l_retained` raw SHA256. The tool rereads and matches all of those facts while
the chip is held in the bootloader, fsyncs staged JSON intent before mutation,
erases only the retained range without rebooting, rereads it before allowing a
hard reset, requires every byte to be `0xFF`, and retains no raw backup.

Marker- or sentinel-owned recovery performs no explicit retained-partition
erase. With only the default sentinel remaining, marker reconstruction proceeds
only after NVS initialization succeeds and the existing dedicated anchor is
verified. If both metadata markers and the default sentinel are lost
simultaneously, including an anchor-only valid NVS, firmware preserves the
region and reports `external_init_required=true`; it does not delete that state
automatically.

Published pre-anchor marker format v1 is an explicit supported upgrade path.
Valid v1 markers prove ownership even for an empty partition, so firmware
initializes it and commits the v2 anchor without erasing retained data, migrates
legacy keys, commits the default sentinel, and only then rewrites both metadata
slots as v2. Release hardware status must show `marker_ready=true`,
`markers_complete=true`, `anchor_ready=true`, `sentinel_ready=true`,
`external_init_required=false`, `ready=true`, and both init and migration errors
as `ESP_OK`. No automatic whole-default-NVS erase is allowed.

`storage status.retained_nvs.telemetry` exposes a boot-local retained-NVS
capacity snapshot plus API-level write, committed-byte, failure, and erase
counters globally and per Public/DM/route/packet store. The capacity fields come
from `nvs_get_stats("d1l_retained", ...)`; a failed capacity read remains
explicit through `capacity.valid=false` and `capacity.error`. These counters
measure requests and successful `nvs_commit` calls, not physical flash
program/erase cycles, so release write-amplification evidence must pair them
with the retained scheduler's dirty/coalesced/commit counts over a named
workload and exact boot nonce. Erase counters cover existing-key mutation and
commit attempts in the dedicated partition; a missing-key no-op or a pre-read
failure does not claim an erase attempt.

`ui_capture_d1l.py` is the hardware display truth path for the split-page UI
blocker. It reads the 480x480 RGB565 frame through the explicitly qualified D1L
target (the `neopi5` stable by-id link for the current route, or `COM12` on the
Windows alternative), writes JSON/PNG/raw artifacts, and must stay
non-destructive: no RF send, no SD format, and no manual touch requirement.

## GitHub Actions

The `d1l-ci` workflow runs host checks on Windows plus ESP32 firmware
build/package generation using the issue #63 selected target,
`espressif/idf:v5.5.4` pinned to its reviewed SHA-256 container digest. The
digest makes the build image content-immutable; the remaining exact-candidate
and physical gates still determine release qualification. The default path
skips SD/RP2040 dry-runs and RP2040 Arduino builds so ESP32/UI fixes do not
rebuild or revalidate the already-working bridge. Expected default artifacts:

- `d1l-host-artifacts`
- `d1l-meshcore-wire-conformance`
- `d1l-idf55-migration-state`
- `d1l-firmware-artifacts`
- `d1l-release-package`

When `include_sd_bridge=true` is selected, or SD/RP2040 paths changed, the
workflow also emits:

- `rp2040-sd-bridge-firmware`
- `rp2040-sd-smoke-firmware`
- `rp2040-seeed-official-sd-smoke-firmware`

`d1l-host-artifacts` includes `ui-sim/` screenshots and `ui-sim-report.json`, including the first-boot onboarding surface.

For qualifying Core evidence, dispatch the exact branch with
`include_sd_bridge=false`, wait for success, and use the strict capture tool.
It rejects pull-request merge SHAs, unexpected archive sets, digest drift, and
an existing destination:

```powershell
gh workflow run d1l-ci.yml --ref release/24h-core -f include_sd_bridge=false
gh run watch <run-id> --exit-status
python .\scripts\capture_core_actions_run_d1l.py `
  --github-run-id <run-id> `
  --commit <40-hex-sha> `
  --github-run-dir artifacts\github\<run-id>-<40-hex-sha>
```

### Issue #63 SDK qualification

Do not generate or repair `dependencies.lock` by hand or with a local firmware
build. During the migration, let ESP-IDF Component Manager generate the lock in
the version-pinned Actions environment. Archive the exact generated lock and
diff, review and commit that output, then rerun Actions and require the lock to
remain unchanged. Retain the run ID, commit, selected image tag, resolved image
identity when Actions exposes it, lock file, package checksums, and artifact
metadata as one evidence set.

After that clean repeat build passes, flash only its verified artifact to the
qualified D1L target: the stable `neopi5` by-id link for the current route, or
`COM12` on the Windows alternative. Run `version` first and require the JSON
response to contain `"idf":"v5.5.4"`, then run the issue #63 board,
display/touch, Wi-Fi, RF,
RP2040/SD, Map, health, reboot, and post-power-cycle checks. Refresh the relevant
commit-matched release-gate evidence before calling v5.5.4 the production
baseline. The `supported_sdk_baseline` audit item checks the workflow selection
and committed lock's IDF version; it does not prove lock provenance or replace
these build and hardware stages.

RP2040 SD bridge UF2 flashing is not an ESP32 `esptool` path. After putting the
D1L RP2040 into UF2/BOOTSEL mass-storage mode, use the guarded helper so the
artifact checksum and target UF2 metadata are verified before any copy:

```powershell
python .\scripts\flash_rp2040_sd_bridge_uf2.py --artifact-dir artifacts\github\<run-id>\rp2040-sd-bridge-firmware --list-volumes
python .\scripts\flash_rp2040_sd_bridge_uf2.py --artifact-dir artifacts\github\<run-id>\rp2040-sd-bridge-firmware --volume <RP2040_UF2_DRIVE>:
python .\scripts\flash_rp2040_sd_bridge_uf2.py --artifact-dir artifacts\github\<run-id>\rp2040-sd-bridge-firmware --volume <RP2040_UF2_DRIVE>: --copy
```

## Release Rules

- Keep flash commands explicit-target only; require stable by-id plus
  `1A86:7523` on `neopi5`, or exact `COM12` on the Windows alternative.
- Keep Wi-Fi/BLE optional and documented when runtime support is disabled.
- Keep full-flash flows behind typed confirmation.
- Update `README.md`, `docs/ROADMAP.md`, `docs/KNOWN_LIMITATIONS.md`, and `docs/RELEASE_CHECKLIST.md` when hardware evidence changes.
- Do not mark the roadmap complete until manual UI review, full DM proof, long soaks, and final release docs/tests are actually complete.
