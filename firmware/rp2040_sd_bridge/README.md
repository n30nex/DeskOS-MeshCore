# DeskOS D1L RP2040 SD Bridge

This Arduino sketch is the RP2040 side of the optional D1L SD-card data bridge.
It speaks the newline-delimited protocol documented in
`docs/SD_BRIDGE_PROTOCOL_D1L.md` over the internal ESP32/RP2040 UART.

## Pin Contract

- RP2040 `Serial1` RX: GPIO17, connected to ESP32 TX GPIO19.
- RP2040 `Serial1` TX: GPIO16, connected to ESP32 RX GPIO20.
- SD SPI: `SPI1`.
- SD detect: GPIO7, reported as raw driven/floating diagnostic state.
- SD CS: GPIO13.
- SD SCK: GPIO10.
- SD MOSI/TX: GPIO11.
- SD MISO/RX: GPIO12.
- SD/sensor rail power enable: GPIO18, driven high before SD init.
- Internal sensor/I2C bus used by Seeed's SD example: SDA GPIO20, SCL GPIO21.
- The first explicit mount attempt follows Seeed's published MicroSD pin/power
  sequence and Arduino-Pico's SdFat SPI1 pin setup: drive GPIO18 high,
  initialize `Wire` on SDA20/SCL21, set `SPI1` SCK/MOSI/MISO to GPIO10/11/12,
  register GPIO13 as CS, then call
  `SD.begin(13, 1000000, SPI1)`. If that first library mount fails, the bridge
  repeats the same official Seeed path once after a clean GPIO18 rail cycle,
  then fails closed with `sd_mount_failed_official_seeed_path`. Manual
  diagnostics can force-cycle the selected
  rail level with the SD SPI pins floated while GPIO18 is off so CS/MOSI/SCK
  cannot backfeed the card, wait for the rail to settle, bias
  CS/MOSI/SCK/MISO, and send idle clocks so warm-reset cards can re-enter SPI
  init. Raw CMD0 reset-entry is sent immediately after the CS-high idle clocks
  without selected-ready pre-clocks; diagnostics report `*_c0r=255` for that
  deliberate no-prewait path. Before those raw probes, the
  bridge repeats the Seeed init once after a
  settled GPIO18 rail cycle so the filesystem stack gets a clean power-on
  attempt. SD MISO uses the
  RP2040 internal pull-up and input buffer before and after SPI1 claims the pin
  so a floating or open card-response line does not read as a false all-zero
  response.
- UART baud: 460800 for bounded ESP32/RP2040 file transfer and command exchange.

The pin values are based on Seeed's SenseCAP Indicator RP2040 Arduino examples.
This bridge code is original project code and intentionally keeps the protocol
plain ASCII rather than Seeed's sensor `PacketSerial` framing.

## Build

Firmware builds are run in GitHub Actions. The workflow installs Arduino CLI,
adds the `earlephilhower/arduino-pico` board package URL, installs pinned
`rp2040:rp2040@5.6.1`, and compiles the production sketch with FQBN
`rp2040:rp2040:seeed_indicator_rp2040:usbstack=nousb` and
`compiler.cpp.extra_flags="-DUSE_SD_CRC=1"`. The current validation card is
user-confirmed FAT32 and accepts raw sector reads only when SD command CRC is
valid, so the bridge stays close to Seeed's documented SD pin/power path while
using Arduino-Pico's second-port SD support,
Seeed's `SD.begin(13, 1000000, SPI1)` sample shape, and the Arduino-Pico
maintainer's SPI1 pin method names used by Seeed's sample: `setCS`, `setRX`,
`setTX`, and `setSCK`.

The production bridge deliberately selects Arduino-Pico's `No USB` stack. Its
only production control/data path is the 460800-baud ESP32 UART, including the
explicit `DESKOS_SD_BOOTLOADER` command. This prevents an unrelated Windows
serial poll from using Arduino-Pico's standard 1200-baud/DTR USB-CDC trigger to
put the RP2040 into UF2 mode and mount an Explorer drive while the bridge is
idle. After a production bridge flash, COM16 is therefore expected to disappear;
prove bridge health through the ESP32 console on COM12. The two isolated SD
smoke sketches retain the normal USB stack because their bounded evidence is
captured directly on COM16 before the production bridge is restored.

Arduino-Pico 5.6.1 bundles SdFat commit
`cda057318bec196183d4cc92b01bc1dd64bbfb02`, whose unused RP2040 PIO-SD source
hardcodes serial debug on even when this bridge uses SPI1. A no-USB link therefore
has no `Serial` object for that dead debug path. The Actions job fail-closed
checks the exact pinned source marker, changes only `USE_DEBUG_MODE` from `1` to
`0`, verifies the replacement, and packages
`sdfat-no-usb-patch.json` with the before/after SHA-256 values. The production
sketch and its SD protocol are not patched by this step.

The bridge emits checksummed artifacts under `rp2040-sd-bridge-firmware`.
The CI job also emits `rp2040-sd-smoke-firmware`, a non-production isolation
sketch that runs only Seeed's published MicroSD init shape and reports
`SEEED_SD_SMOKE ... public_rf_tx=0 formats_sd=0`.
The stricter release proof artifact is
`rp2040-seeed-official-sd-smoke-firmware`; its sketch emits
`{"test":"seeed_official_sd_smoke",...}` after the exact Seeed GPIO18,
Wire20/21, SPI1 10/11/12, CS13, 1 MHz sequence and proves root open,
mkdir, write, read, rename, stat, delete, FAT32, <=32GB, no Public RF, and no
formatting. The same JSON line also carries non-formatting raw diagnostics
(`raw_cmd0`, `raw_cmd8`, `raw_r70`..`raw_r73`, `raw_acmd41`, `raw_ocr0`..`raw_ocr3`,
MISO samples, and GPIO7 detect samples). `power_state` is deliberately reported
as `gpio18_commanded_high_not_measured` until a meter or scope artifact proves
the socket rail under load.
Do not use the Windows host for firmware compilation.

## Hardware Validation

Verify the GitHub Actions artifact checksum before any RP2040 flash attempt:

```powershell
python .\scripts\verify_checksums.py artifacts\github\<run-id>\rp2040-sd-bridge-firmware
python .\scripts\flash_rp2040_sd_bridge_uf2.py --artifact-dir artifacts\github\<run-id>\rp2040-sd-bridge-firmware --list-volumes
```

The current ESP32 release flashing scripts are not RP2040 UF2 flashing tools.
If the already-flashed bridge answers `DESKOS_SD_PING`, the ESP32 console can
request UF2 mode with `rp2040 bootloader` / `DESKOS_SD_BOOTLOADER` without
touching SD. Otherwise put the D1L RP2040, not the ESP32-S3, into UF2/BOOTSEL
mass-storage mode before copying `deskos_sd_bridge.ino.uf2`. Use
`scripts/flash_rp2040_sd_bridge_uf2.py --volume <RP2040_UF2_DRIVE>:` first as a
dry run, then add `--copy` only after it verifies the artifact checksum and UF2
bootloader metadata. Use an empty or sacrificial SD card first: `DESKOS_SD_STATUS`
is non-formatting, but a mounted usable filesystem may get a `/deskos`
directory.

After flashing the RP2040 bridge, validate through the ESP32 console on COM12:

```powershell
python .\scripts\rp2040_sd_bridge_preflight_d1l.py --port COM12 --artifact-dir artifacts\github\<run-id>\rp2040-sd-bridge-firmware
python .\scripts\sd_file_canary_d1l.py --port COM12
python .\scripts\sd_retained_history_acceptance_d1l.py --port COM12
python .\scripts\soak_d1l.py --port COM12 --duration-sec 90 --sample-interval-sec 30 --sample-storage --sd-file-canary
python .\tools\rp2040_sd_protocol.py --request DESKOS_SD_PING
python .\tools\rp2040_sd_protocol.py --scenario ready --file-canary-transcript
```

The preflight sends `rp2040 ping` before `storage status`, so UART protocol
health can be proven without touching SD. It then uses explicit `storage mount`
for the SD-touch attempt and sends `storage diag` as optional non-destructive
evidence when the flashed bridge supports `DESKOS_SD_DIAG`. The canary sends
`storage status`, `storage filecanary`, `storage status`,
`packets`, and `health`. It does not send Public RF or any formatting command.
The SD-aware soak repeats `storage status` and
`storage filecanary` during a passive stability window. The retained-history
acceptance runner seeds synthetic Public/DM/route/packet rows without Public RF,
reboots, and verifies those rows are still readable. The protocol transcript
command is host-only and prints the deterministic `DESKOS_SD_FILE v=1` request
sequence that mirrors the ESP32 `storage filecanary` path.

See `docs/RP2040_SD_BRIDGE_FLASH_D1L.md` for the full flash/proof runbook.

## Runtime Notes

- `DESKOS_SD_STATUS` is safe to call during boot and UI polling. It does not
  probe, mount, format, or write SD. Before an explicit mount it reports
  `state=mount_required`; after `DESKOS_SD_MOUNT` or file operations, it
  returns the cached latest SD state. Ready cached cards
  advertise `file_ops=1`, `file_line_max=512`, `file_chunk_max=192`,
  `path_max=96`, and `atomic_rename=1`. Status replies include cached hardware
  context. The current ESP32 parser requires `probe_power`,
  `probe_mode`, `probe_err`, `probe_data`, `mount_err`, and `mount_data` so a
  partial/prefix-only reply cannot look ready. `probe_present` and the raw
  GPIO7 tokens `detect`, `detect_driven`, `det_pullup`, and `det_pulldown` are
  additional diagnostics and are not part of the ESP32 ready decision. Older
  bridge builds that omit required fields fail closed and must be upgraded.
  When a successful mounted snapshot has already proven the inserted-card
  signature (`detect=low detect_driven=1`), status polling also samples only
  GPIO7. Three consecutive samples that no longer match that proven signature
  trigger one bounded read-only `CMD17` transaction for sector zero on the
  idle shared SPI bus. A complete sector with a valid data token and CRC16
  rejects the GPIO-only removal as noise and keeps the ready snapshot. Only a
  failed or corrupt transaction invalidates the cached mount and reports
  `state=no_card note=card_removed`.
  The bridge retains only that proven detect signature; three consecutive
  matching samples after removal publish
  `state=mount_required note=card_reinserted`, allowing the ESP32 storage
  manager to request a fresh mount without the status poll touching the bus.
  This bounded runtime-removal check never formats or writes and never assumes
  a detect polarity before a successful mount has established it. Routine
  status remains bus-silent; only a fully debounced removal suspicion performs
  the single sector read, with 300 ms ready and data-token timeouts.
- `DESKOS_SD_MOUNT` is the deliberate SD-touch request used by `storage mount`.
  It runs on the protocol-handling core because the Arduino `SD`/`SDFS`
  filesystem stack can wedge when invoked from the RP2040 core1 worker. The
  command first tries the already-powered high/dedicated Arduino-Pico SPI1
  path with CS idled high and GPIO18 settled, registering GPIO13 as SPI1 CS,
  and calling `SD.begin(13, 1000000, SPI1)` without running a second SdFat
  probe on failure. If that already-powered library path does not mount, the
  bridge repeats the same Seeed path once after cycling GPIO18 with the SD SPI
  pins floated during rail-off. If both official attempts fail, production
  mount returns `state=error note=sd_mount_failed_official_seeed_path`;
  heuristic raw probes remain diagnostic-only under `DESKOS_SD_DIAG`. A mounted
  filesystem is rejected before `/deskos` creation unless `SD.fatType()==32`,
  and reports `not_fat32_or_unmountable` with `needs_fat32=1`. Users must
  prepare FAT32 cards on a computer.
  If a ready snapshot is already cached, an explicit mount request performs a
  real close/power-cycle/remount validation instead of returning the cached
  result. A debounced `no_card` remains cached and bus-silent until the proven
  detect signature returns, so an empty-slot remount cannot erase reinsertion
  monitoring.
- `DESKOS_SD_PING` reports protocol/file-operation limits and `sd_touch=0`
  without probing, mounting, formatting, or writing SD. It advertises
  `stream_write=1` when the fail-closed streaming put operations are available.
  ESP32 exposes this as `rp2040 ping` for bridge-app validation before any
  SD-specific request.
- `DESKOS_SD_BOOTLOADER` replies with `ok=1`, `sd_touch=0`,
  `public_rf_tx=0`, and `formats_sd=0`, then calls the Arduino-Pico
  `rp2040.rebootToBootloader()` API so autonomous validation can copy a UF2
  without a BOOTSEL button press when a compatible bridge is already running.
- `DESKOS_SD_DIAG` is a manual diagnostic request used by `storage diag`. It
  reports the pin contract, selected rail/SPI mode, and the high/dedicated,
  high/shared, low/dedicated, low/shared, and high-power bit-banged raw probe
  result. It returns a
  pending-shaped diagnostic line instead of blocking the UART while another SD
  worker is running, uses only the bounded raw SPI probe, is non-formatting, and
  does not write to the card. Probe tokens include SPI pin-acceptance flags,
  the CMD0 and CMD8 selected-ready bytes, raw `CMD0`, `CMD8`, R7
  echo bytes (`*_c0r`, `*_c8r`, `*_c0`, `*_c8`, `*_r70`..`*_r73`), MISO line samples
  (`*_miso_pull`, `*_miso_spi`, `*_miso_idle`), and the first
  CS-high idle transfer byte (`*_idle_ff`) so all-zero bus behavior can be
  separated from a real card echo mismatch. The `bb_*` fields bypass the SPI1
  peripheral with direct GPIO clocking and are diagnostic-only; they do not
  enable file operations. Diagnostic replies also include
  raw GPIO7 detect samples so hardware insert-detect behavior can be correlated
  with the SPI response path. The raw probe sends CMD0 without a selected-ready
  prewait, then scans past leading all-zero CMD0 response bytes inside the
  response window. A `CMD0=0` result is allowed to continue only far enough to
  require a valid CMD8 echo and ACMD41 completion, so a stuck-low DO line is
  not treated as a mounted card. The bit-banged probe also retries all-zero
  CMD0 results with one to seven pre-command clocks while CS is asserted. `bi_*` repeats the bit-banged
  probe with inverted CS idle/select polarity to rule out a board-select
  polarity or routing mismatch without driving MISO. `bs_*`, `bcm_*`, and
  `bsc_*` repeat the bit-banged probe with SCK/MOSI, CS/MOSI, and SCK/CS
  swapped respectively, which exercises only card input lines and rules out
  those safe pin-map mismatches. These diagnostics test
  reset-entry byte recovery without formatting or writing the card.
- The bridge has no SD formatting command. If a FAT32 card is mounted and the
  `/deskos` structure is missing, the bridge creates the DeskOS directories and
  manifests. If an existing DeskOS manifest is invalid, the bridge preserves it
  as `.bad`, reports `deskos_manifest_invalid`, and keeps file operations
  disabled. If the card is absent, unmountable, not FAT32, or has invalid
  DeskOS manifests, the ESP32 keeps NVS fallback active and tells the user what
  to fix on a computer.
- `DESKOS_SD_FILE v=1 ...` provides bounded generic file operations under
  `/deskos`: `stat`, `read`, `write`, `append`, `delete`, and `rename`. Payloads
  use base64url without padding, CRC32 checks, sanitized relative paths, and
  192-byte chunks so each newline-delimited request remains under the 512-byte
  line cap. `rename replace=1` uses a backup/rollback step when replacing an
  existing final file so ordinary rename failures do not erase the old final.
  The full-path buffer is sized for a maximum 96-byte relative path plus the
  internal `.bak` suffix used by that rollback step.
- The same v1 file protocol provides `put_begin`, `put_chunk`, `put_end`, and
  `put_abort` for files up to 256 KiB. A streaming put opens and truncates its
  target once, requires exact sequential offsets and a CRC32 on every chunk,
  then flushes, closes, and verifies the complete size and CRC32 by reading the
  file locally on the RP2040. Only that fully verified `put_end` returns
  success. One session may be active; other file operations fail with `busy`,
  and session operations without one fail with `no_session`. Abort closes and
  removes the uncommitted partial target when present. Terminal write and
  verification failures do the same and report whether removal was confirmed.
  A failed removal retains cleanup-only ownership so `put_abort` can retry;
  no ordinary file operation can observe or commit that partial target through
  the bridge session.
- No formatting happens at boot, ping, status, mount, diagnostics, or file
  checks.
- Retained Public message history, DM history, route history, and packet history
  can use the SD file protocol once the ESP32 sees a ready card, file
  operations, and atomic rename. The ESP32 keeps onboard NVS mirrors for these retained
  history stores. Diagnostic exports, sampled user-data exports under
  `exports/data`, and the map-tile cache can use the same file gate. Settings,
  identity, contacts, read-state, crashlog, and the full map page/tile download
  policy remain onboard/fallback-backed or pending.
