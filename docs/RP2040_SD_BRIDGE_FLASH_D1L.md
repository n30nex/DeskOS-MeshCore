# D1L RP2040 SD Bridge Flash And Proof

This procedure is for the D1L RP2040 SD bridge firmware only. It is not the
ESP32-S3 DeskOS firmware flash path.

## Inputs

- Use a GitHub Actions `rp2040-sd-bridge-firmware` artifact. Do not build the
  RP2040 bridge on the Windows host.
- Use the same Actions run's `rp2040-seeed-official-sd-smoke-firmware` artifact
  for the isolated official Seeed SD proof before treating SD as release-ready.
- Use a FAT32 SD card prepared on a computer. `DESKOS_SD_STATUS` does not
  format, and a usable card may get a `/deskos` directory during explicit mount.
- Do not format from ESP32 firmware, RP2040 firmware, serial commands, scripts,
  or UI.
- Use `COM12` only for post-flash ESP32 serial validation.
- Do not use COM8, COM11, or COM29 for this work.
- Do not send Public RF.

## Verify Artifact

```powershell
python .\scripts\verify_checksums.py artifacts\github\<run-id>\rp2040-sd-bridge-firmware
python .\scripts\verify_checksums.py artifacts\github\<run-id>\rp2040-seeed-official-sd-smoke-firmware
Get-FileHash artifacts\github\<run-id>\rp2040-sd-bridge-firmware\deskos_sd_bridge.ino.uf2 -Algorithm SHA256
python .\scripts\flash_rp2040_sd_bridge_uf2.py --artifact-dir artifacts\github\<run-id>\rp2040-sd-bridge-firmware --expected-sha256 <sha256> --list-volumes
python .\scripts\rp2040_sd_bridge_preflight_d1l.py --port COM12 --artifact-dir artifacts\github\<run-id>\rp2040-sd-bridge-firmware --expected-sha256 <sha256> --out artifacts\rp2040-preflight\d1l-rp2040-sd-bridge-preflight-COM12.json
```

## Guided Manual Install

If COM12 is healthy but the RP2040 does not answer `rp2040 ping` and no
autonomous COM16/UF2 path appears, use the guided installer instead of looping
autonomous validation:

```powershell
python .\scripts\guided_sd_install_d1l.py --github-run-id <run-id> --github-run-dir artifacts\github\<run-id>-current --commit <sha> --d1l-port COM12 --rp2040-port COM16
```

The guided flow is documented in `docs/D1L_SD_CARD_GUIDED_INSTALL.md`. It still
verifies Actions-built checksums, refuses COM8/COM11/COM29, never formats SD, never
sends Public RF, captures the official Seeed SD smoke proof, restores the
DeskOS RP2040 bridge, runs COM12 preflight, and runs short SD canaries when the
file gate is ready. The only manual operation is putting the RP2040 into
UF2/BOOTSEL mode when prompted.

The last hardware-tested UF2 before the safe-status/explicit-mount follow-up was
from Actions run `28499319258` for commit
`a8268073ae290567e26f27491c8ffa167f6f8d57`:

```text
AA71CD32C9433F1D57B1C3F243ABB2A7535728E6A3905C6A424A1E77D5F3E57E
```

Earlier Actions artifact `28494746866` was copied once after the RP2040 UF2
volume mounted as `RPI-RP2` at `G:\`. Post-copy COM12 preflight then reported
`state="rp2040_protocol_pending"` and `storage status` / `storage diag`
timeouts, so follow-up bridge changes were built by Actions. Run `28499319258`
flashed the ESP32 image and copied the RP2040 UF2 after the ESP32
`rp2040 reset` command cleared the RP2040 USB/CDC wedge; COM12 then proved
`rp2040 ping` works with `sd_touched=false`, but the SD-touching status/diag
path still timed out. Current preflight therefore includes `rp2040 ping` to
prove the flashed bridge app answers without touching SD, then uses explicit
`storage mount` for the direct SD-touch attempt and `storage remount` in the
acceptance runners to prove storage-manager convergence.

The preflight command is non-destructive. It verifies the RP2040 artifact when
provided, lists UF2 bootloader volumes, queries only the selected D1L serial
port with `rp2040 status`, `rp2040 ping`, safe `storage status`, explicit
`storage mount`, bounded safe-status polling while SD reports
`state="mount_pending"` or the manager reports `BRIDGE_WAIT`, `PING`, `STATUS`,
or `MOUNT`,
optional `storage diag`, and `health`,
and reports the next safe action as JSON. Acceptance requires both the fresh
FAT32 file-operation gate and `manager.state="READY_SD"`; exhausted manager
transitions set top-level `ok=false`, return nonzero, and remain fail-closed with
separate total/mount/manager poll counts plus exhaustion fields, even if the
serial commands themselves succeeded. `rp2040 ping` must report
`sd_touched=false`; `storage mount` and `storage diag` are non-formatting and
may be unavailable on older bridge firmware. If preflight reports
`state="rp2040_protocol_pending"` or `state="sd_card_not_present_diag_pending"`
and no UF2 volume is available, put the RP2040 into UF2/BOOTSEL mode before
running the copy helper. If it reports `state="sd_mount_required"`, run the
explicit mount path. If it reports `state="sd_mount_pending"` or
`state="sd_status_pending"` after a successful ping, keep the current UF2
installed and inspect the status/mount path instead of copying the same UF2
again.

## Flash

The legacy autonomous refresh is planning-only until its hardware path has
full-key D1L admission and stable flash binding. It can be inspected without
hardware access:

```powershell
python .\scripts\autonomous_hardware_validate_d1l.py --github-run-id <run-id> --github-run-dir artifacts\github\<run-id>-current --commit <sha> --refresh-rp2040-smoke --dry-run
```

The dry-run plan describes how the retired refresh runner bound the
host-success marker and release manifest to
the requested canonical 40-hex commit and explicitly supplied numeric Actions
run, and verifies both packaged and standalone firmware hashes. It touches only
COM12 and, during intentional smoke/UF2 maintenance, COM16; it refuses
COM8/COM11/COM29, flashes the exact ESP32 image, then performs a short RP2040
access precheck before any RP2040 UF2 copy. The production bridge is built with
`usbstack=nousb`, so COM16 is expected to be absent while it runs and a
background 1200-baud serial poll cannot reopen UF2. The precheck lists UF2
volumes, checks whether COM16 is present, asks COM12 for `rp2040 ping`, tries a precise
`rp2040 double-reset` bootloader-entry pattern, then tries one `rp2040 reset`,
and fails closed if no autonomous bootloader path is available. An absent COM16
is accepted only when the COM12 bridge protocol and explicit bootloader command
are available. It does not
format SD, does not send Public RF, and does not require user action.
Pre-existing UF2 disks require explicit `--uf2-volume`; automatic selection is
limited to exactly one newly appeared volume correlated with the commanded D1L
transition. COM17 and other discovered RP2040-looking ports are reported only
as read-only inventory; configured COM16 is the only RP2040 serial port the
runner may touch when USB smoke or maintenance firmware is active. It is not
required while the production no-USB bridge answers through COM12. When access
is available, the runner flashes the official Seeed SD smoke UF2,
captures its COM16 JSON, restores the exact production bridge UF2, and performs
a second checksum-verified, non-erasing exact ESP32 flash. That fresh ESP32 boot
is a required boundary because the intentional bridge-unavailable smoke window
can latch retained-store I/O failures; failure stops before preflight. Raw
diagnostics then run under a bounded deadline as an isolated maintenance phase,
but only after a clean `READY_SD` zero-counter preflight. Before any SD canary,
the runner restores that exact bridge again, reflashes the
exact ESP32 project image, and requires a fresh `READY_SD` preflight with zero
retained failure counters or degradation latches. Any diagnostic or clean
re-entry failure stops before canaries and remains visible to the release gate.
Any later SD-stage failure also stops subsequent stages, preserves its receipt,
runs the release audit, and attempts bounded exact bridge/ESP32 recovery.
Targeted UI corruption and scroll probes are opt-in with `--include-ui-probes`.

For ESP32-side fixes after the RP2040 bridge is already validated, add
`--skip-sd-suite --include-ui-probes` for UI-only validation that flashes only
the ESP32 artifact on COM12. Any run that keeps the SD suite enabled performs
the mandatory exact bridge pre/post-diagnostic restore boundary;
`--refresh-rp2040-smoke` additionally enables official Seeed smoke evidence.

For the SD hardware proof, first flash the verified
`rp2040-seeed-official-sd-smoke-firmware` UF2, capture the emitted JSON under
`artifacts\hardware\com16\seeed_official_sd_smoke_<sha>.json`, and require
`test="seeed_official_sd_smoke"`, `ok=true`, `mount/root_open/mkdir/write/read/rename/stat/delete=true`,
`fat32=true`, `fat_type=32`, `max_card_gb<=32`, `public_rf_tx=false`, and
`formats_sd=false`. Failed smoke captures must preserve the raw diagnostic
fields (`diag_ran`, `detect`, `raw_cmd0`, `raw_cmd8`, `raw_r70`..`raw_r73`,
`raw_acmd41`, and OCR/MISO samples) so the next action can distinguish power,
CS, command-response, CMD8 echo, ACMD41, and filesystem failures. `power_state`
means GPIO18 was commanded high; it is not a substitute for the separate SD
socket voltage/signal measurement artifact. If the autonomous runner fails
before capture starts, it must still archive the same COM16 smoke path as an
`ok=false` artifact with the exception text, `public_rf_tx=false`, and
`formats_sd=false`. Then flash the production bridge artifact below.

1. If the current bridge answers `rp2040 ping`, run `rp2040 bootloader` from
   COM12 to request UF2 mode without touching SD. If the bridge does not answer
   and no COM16/UF2 path is visible, the software has no autonomous BOOTSEL
   control on this board revision.
2. Put the D1L RP2040, not the ESP32-S3, into UF2/BOOTSEL mass-storage mode.
3. Confirm Windows mounted a UF2 bootloader volume. The volume should expose
   UF2 bootloader metadata such as `INFO_UF2.TXT` or `INDEX.HTM`.
4. Dry-run the guarded copy helper. This verifies `SHA256SUMS.txt`, confirms
   the target has UF2 bootloader metadata, and refuses ambiguous/missing volumes:

   ```powershell
   python .\scripts\flash_rp2040_sd_bridge_uf2.py --artifact-dir artifacts\github\<run-id>\rp2040-sd-bridge-firmware --volume <RP2040_UF2_DRIVE>: --expected-sha256 <sha256>
   ```

5. Copy only `deskos_sd_bridge.ino.uf2` with the explicit `--copy` flag:

   ```powershell
   python .\scripts\flash_rp2040_sd_bridge_uf2.py --artifact-dir artifacts\github\<run-id>\rp2040-sd-bridge-firmware --volume <RP2040_UF2_DRIVE>: --expected-sha256 <sha256> --copy --out artifacts\rp2040-flash\rp2040-sd-bridge-uf2-copy.json
   ```

6. Wait for the volume to disconnect/reboot. The production bridge intentionally
   does not enumerate COM16; verify `rp2040 ping` through COM12 instead. Power-cycle
   the D1L only if that UART proof does not recover.

Do not use `flash_d1l.ps1`, `flash_project.ps1`, or `esptool` for this RP2040
step; those are ESP32-S3 paths.

## Prove

Run these from the repo root after the RP2040 reboots:

```powershell
python .\scripts\rp2040_sd_bridge_preflight_d1l.py --port COM12 --artifact-dir artifacts\github\<run-id>\rp2040-sd-bridge-firmware --out artifacts\rp2040-preflight\d1l-rp2040-sd-bridge-postflash-COM12.json
python .\scripts\smoke_d1l.py --port COM12 --out artifacts\smoke\d1l-smoke-rp2040-sd-bridge-COM12.json
python .\scripts\sd_boot_prepare_acceptance_d1l.py --port COM12 --scenario correct-structure --out artifacts\sd-boot-prepare\d1l-sd-boot-correct-structure-COM12.json
python .\scripts\sd_boot_prepare_acceptance_d1l.py --port COM12 --scenario missing-structure --out artifacts\sd-boot-prepare\d1l-sd-boot-missing-structure-COM12.json
python .\scripts\sd_file_canary_d1l.py --port COM12 --out artifacts\sd-canary\d1l-sd-file-canary-COM12.json
python .\scripts\sd_export_canary_d1l.py --port COM12 --token export1 --out artifacts\sd-export-canary\d1l-sd-export-canary-COM12.json
python .\scripts\sd_diagnostic_export_d1l.py --port COM12 --token diag1 --out artifacts\sd-diagnostic-export\d1l-sd-diagnostic-export-COM12.json
python .\scripts\sd_data_export_d1l.py --port COM12 --token data1 --out artifacts\sd-data-export\d1l-sd-data-export-COM12.json
python .\scripts\sd_map_tile_canary_d1l.py --port COM12 --token map1 --out artifacts\sd-map-tile-canary\d1l-sd-map-tile-canary-COM12.json
python .\scripts\sd_retained_history_acceptance_d1l.py --port COM12 --out artifacts\sd-retained-history\d1l-sd-retained-history-COM12.json
python .\scripts\soak_d1l.py --port COM12 --duration-sec 90 --sample-interval-sec 30 --sample-storage --sd-file-canary --out artifacts\soak\d1l-passive-soak-rp2040-sd-bridge-COM12.json
```

Expected proof with a ready card:

- `storage status` reports `sd.rp2040_protocol_supported=true`.
- `storage remount` returns `ok=true`, `storage status` exposes a
  `manager.state`, and the ready SD state is visible before file canaries are
  attempted.
- `sd_boot_prepare_acceptance_d1l.py` reports ready file gates for correct and
  missing-structure FAT32 cards without formatting. Unmountable or non-FAT32
  media must remain on NVS fallback and tell the user to prepare FAT32 on a
  computer.
- `rp2040 ping` reports `ok=true`, `protocol_supported=true`,
  `sd_touched=false`, `public_rf_tx=false`, and `formats_sd=false`.
- `sd.file_ops=true`, `sd.atomic_rename=true`, `sd.file_line_max >= 512`,
  `sd.file_chunk_max >= 192`, and `sd.path_max >= 96`.
- `message_store_backend="sd"`, `dm_store_backend="sd"`,
  `route_store_backend="sd"`, `packet_log_backend="sd"`,
  `data_backend="mixed"`, and `setup_action="retained_history_sd_enabled"`.
- `retained_sd.degraded=false` and each retained store reports zero
  `sd_read_fail_count`, `sd_write_fail_count`, and `sd_rename_fail_count`
  before canaries; if the card is yanked or SD writes fail, `storage status`
  must show `SD degraded; using internal fallback` while new rows remain
  available from onboard NVS fallback.
- `storage filecanary` returns `ok=true`, `rename_replace=true`,
  `read_final=true`, `delete_final=true`, and `stat_deleted=true`.
- `storage export-canary <token>` returns `ok=true`, `write_tmp=true`,
  `read_tmp=true`, `rename_replace=true`, `stat_final=true`, `read_final=true`,
  leaves `exports/diagnostics/export-canary-<token>.json` present, and reports
  `public_rf_tx=false` plus `formats_sd=false`.
- `storage export-diagnostics <token>` returns `ok=true`, writes a chunked
  diagnostic bundle to `exports/diagnostics/diagnostic-export-<token>.json`,
  verifies temp and final readback, leaves the final JSON present, and reports
  `public_rf_tx=false` plus `formats_sd=false`.
- `storage export-data <token>` returns `ok=true`, writes a chunked sampled
  user-data bundle to `exports/data/data-export-<token>.json`, verifies temp
  and final readback, leaves the final JSON present, reports
  `private_identity_exported=false`, and reports `public_rf_tx=false` plus
  `formats_sd=false`.
- `storage map-tile-canary <token>` returns `ok=true`, writes
  `map/tiles/z12/x1/y2-<token>.tile` through temp write/read and atomic rename,
  leaves the final synthetic tile present, and reports `public_rf_tx=false`
  plus `formats_sd=false`. This is a storage-only synthetic canary and never
  requests a network tile. Live Map acceptance is separate and remains limited
  to the visible current-view 3x3 at one zoom per visible generation while the
  actual Map is active. User pan/zoom may select another current view,
  but never starts background or multi-zoom prefetch.
- `storage retained-canary <token>` returns `ok=true`, appends synthetic Public,
  DM, route, and packet rows without Public RF or formatting, and
  `sd_retained_history_acceptance_d1l.py` proves those rows are readable before
  and after a reboot.
- The passive soak reports zero active Public TX, zero crash-like resets, and
  monotonic uptime, plus stable SD state/backend samples and repeated passing
  `storage filecanary` results.

Expected proof before the RP2040 bridge is flashed, with no card, or with an
unsupported card is a safe refusal: onboard NVS remains the fallback and no
format is performed. For that pre-flash/fallback state only, the soak command
may be run with `--allow-sd-unavailable` so the expected `storage filecanary`
preflight refusal is recorded without failing the passive stability window:

```powershell
python .\scripts\soak_d1l.py --port COM12 --duration-sec 90 --sample-interval-sec 30 --sample-storage --sd-file-canary --allow-sd-unavailable --out artifacts\soak\d1l-passive-soak-sd-aware-pre-rp2040-flash-COM12.json
```

The host simulator can print the deterministic filecanary request/reply grammar
without hardware:

```powershell
python .\tools\rp2040_sd_protocol.py --scenario ready --file-canary-transcript
python .\tools\rp2040_sd_protocol.py --scenario ready --map-tile-canary-transcript --token map1
```
