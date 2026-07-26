# D1L RP2040 SD Bridge Protocol

> **HISTORICAL PROTOCOL/INTEGRATION NOTES — NOT CURRENT STORAGE POLICY.**
> The wire-format observations remain useful, but statements below that route
> retained history to NVS are superseded by RC1 SD-primary/live-only degraded
> behavior. Use the [project README](../README.md),
> [RC1 user guide](USER_GUIDE_D1L.md), [RC1 test plan](TEST_PLAN_D1L.md), and
> [SD-card guide](D1L_SD_CARD_GUIDED_INSTALL.md).

This is the DeskOS line protocol expected on the ESP32-S3 to RP2040 UART for optional D1L SD-card data storage.

The ESP32 side uses raw newline-delimited ASCII at the board UART pin contract documented in `boards/seeed_indicator_d1l/pinmap.json`: ESP32 UART2 TX GPIO19, ESP32 UART2 RX GPIO20, 115200 baud. The RP2040-side Arduino target lives at `firmware/rp2040_sd_bridge/deskos_sd_bridge`, and `tools/rp2040_sd_protocol.py` is the host reference simulator for this contract.

## Status Request

ESP32 sends:

```text
DESKOS_SD_STATUS
```

RP2040 replies with one line:

```text
DESKOS_SD_STATUS state=ready present=1 mounted=1 deskos=1 fs=fat32 needs_fat32=0 capacity_kb=31166976 free_kb=31100000 note=ready probe_power=high probe_mode=mount probe_present=1 probe_err=0 probe_data=0 detect=low detect_driven=1 det_pullup=0 det_pulldown=0 mount_err=0 mount_data=0 file_ops=1 file_line_max=512 file_chunk_max=192 path_max=96 atomic_rename=1
```

Required tokens:

- `state`: stable machine state. Use `mount_required`, `mount_pending`, `no_card`, `not_fat32_or_unmountable`, `creating_deskos_files`, `deskos_manifest_invalid`, `ready`, or `error` when possible.
- `present`: `1` when a card is electrically present.
- `mounted`: `1` when the filesystem is mounted and usable.
- `deskos`: `1` when the `/deskos` data root exists or has been created.
- `fs`: filesystem label such as `fat32`, `fatfs`, `exfat`, `unknown`, or `none`.
- `needs_fat32`: `1` when a card is present but cannot be mounted as a usable FAT32 DeskOS card. If `mount_err` or `mount_data` is nonzero on a user-confirmed FAT32 card, treat the result as an RP2040 firmware mount-path blocker rather than a card-format request.
- `capacity_kb` and `free_kb`: unsigned decimal kilobytes, `0` when unknown.
- `probe_power`, `probe_mode`, `probe_present`, `probe_err`, and
  `probe_data`: non-formatting card-probe diagnostics. `mount_err` and
  `mount_data` are captured SdFat diagnostic error bytes from the filesystem
  mount attempt. The current ESP32 parser requires `probe_power`, `probe_mode`,
  `probe_err`, `probe_data`, `mount_err`, and `mount_data`; a bridge build that
  omits them fails closed as a stale/malformed exchange and must be upgraded.
  `probe_present` remains supplemental and is not consumed by the parser.
- `detect`, `detect_driven`, `det_pullup`, and `det_pulldown`: raw GPIO7
  SD-detect diagnostics. The bridge samples GPIO7 with pull-up and pull-down so
  operators can distinguish a floating detect line from a board-driven high or
  low state without assuming a card-format problem. These tokens are
  supplemental; the ESP32 storage parser does not use them to enable stores.
- `file_ops`: `1` when the card is ready and the generic file protocol below is available.
- `file_line_max`: maximum request/reply line length, excluding newline. Current value: `512`.
- `file_chunk_max`: maximum decoded read/write/append payload size. Current value: `192` bytes.
- `path_max`: maximum decoded relative path length. Current value: `96` bytes.
- `atomic_rename`: `1` when temp-file writes can be committed with `rename replace=1`.
  The RP2040 bridge must preserve the previous final file on ordinary rename
  failure when replacing an existing target; power-loss atomicity is still
  limited by the underlying FAT/SD stack.

Values must not contain spaces. Use underscores in `note`.

On the D1L RP2040 bridge, status is safe to call from boot and UI polling. It
does not probe, mount, format, or write SD. Before any explicit mount result is
cached, the bridge reports `state=mount_required note=mount_not_checked`.
After `DESKOS_SD_MOUNT` or file operations complete, status returns the cached
latest SD state. After a successful mounted snapshot has established the
inserted-card GPIO7 signature as `detect=low detect_driven=1`, status polling
uses only that detect pin for routine runtime-removal monitoring. Three
consecutive nonmatching samples trigger one bounded, read-only `CMD17` sector
zero transaction on the idle shared SPI bus. A complete sector with a valid
data token and CRC16 rejects the GPIO-only signal as noise and preserves the
ready snapshot; only a failed or corrupt transaction invalidates the cache and
returns `state=no_card note=card_removed`.
Three consecutive returns to the proven signature publish
`state=mount_required note=card_reinserted`; the ESP32 can then issue a fresh
explicit mount. Routine status is bus-silent; the debounced suspicion path has
300 ms ready and data-token bounds and never formats or writes. Neither path
infers a polarity before a successful mount proves the board signature.

## Mount Request

ESP32 sends this explicit SD-touch command when the operator runs
`storage mount` or when the storage manager services `storage remount`:

```text
DESKOS_SD_MOUNT
```

RP2040 replies with one status-shaped line using the mount prefix after the
explicit SD-touch attempt completes. `DESKOS_SD_STATUS` remains safe and
non-touching before this command, and returns the cached result after this
command completes. When a ready snapshot is already cached, this explicit
request closes and reinitializes the filesystem path rather than accepting the
cache as proof; it is therefore the recovery path for a stale SD session. A
debounced `state=no_card` is returned without touching the bus until GPIO7
confirms reinsertion and status advances to `mount_required`.

The RP2040 bridge is built by GitHub Actions with the Arduino-Pico board
package's default SD library settings. The filesystem path runs on the
protocol-handling core because Arduino `SD`/`SDFS` can wedge when invoked from
the RP2040 core1 worker. It first tries the already-powered high/dedicated
Arduino-Pico SPI1 path using Seeed's published MicroSD sample sequence:
drive GPIO18 high, wait for the rail to settle, initialize `Wire` on
SDA20/SCL21, set `SPI1` SCK/MOSI/MISO to GPIO10/11/12, register GPIO13 as CS,
then call `SD.begin(13, 1000000, SPI1)`. This first attempt intentionally
avoids explicit `SPI1.begin()` or a second SdFat probe before failure handling.
If that already-powered library path does not mount, the bridge repeats the
same Seeed path once after cycling GPIO18 with SD SPI pins floated during
rail-off so CS/MOSI/SCK cannot backfeed the card. If both official attempts
fail, the bridge reports
`state=error note=sd_mount_failed_official_seeed_path` with the captured mount
error bytes. It deliberately does not turn a cold mount failure into strict
`no_card`: an absent card and an inserted-but-unresponsive card are not safely
distinguishable from the filesystem failure alone, and a false absence would
erase the ESP32's last confirmed presence. Operators can request the separate,
non-writing `DESKOS_SD_DIAG` raw-probe matrix when diagnosing that error.

```text
DESKOS_SD_MOUNT state=ready present=1 mounted=1 deskos=1 fs=fat32 needs_fat32=0 capacity_kb=31166976 free_kb=31100000 note=ready probe_power=high probe_mode=mount probe_present=1 probe_err=0 probe_data=0 detect=low detect_driven=1 det_pullup=0 det_pulldown=0 mount_err=0 mount_data=0 file_ops=1 file_line_max=512 file_chunk_max=192 path_max=96 atomic_rename=1
```

The D1L bridge tries the Seeed-style high/dedicated filesystem mount once with
the already-powered rail and once after a bounded power cycle. A mounted
non-FAT32 filesystem reports `state=not_fat32_or_unmountable`, `present=1`,
`mounted=1`, `needs_fat32=1`, and `note=needs_fat32_on_computer`; the ESP32
keeps NVS fallback active. A failed mount remains `state=error`, even when the
reply's instantaneous `present` field is `0`; only the debounced runtime
removal of a previously ready card emits strict `state=no_card`. The ESP32 must
therefore preserve last-confirmed presence across error replies and surface
`inspect_rp2040_sd_mount_error_firmware_path` when `mount_err` or `mount_data`
is nonzero on a user-confirmed FAT32 card instead of telling the user to
prepare another card. The manual diagnostic request below remains available
for the broader raw probe matrix without filesystem writes.

## Ping Request

ESP32 sends this fast protocol sanity check before SD-specific preflight work:

```text
DESKOS_SD_PING
```

RP2040 replies with one line and must not probe, mount, format, or write SD:

```text
DESKOS_SD_PING v=1 file_line_max=512 file_chunk_max=192 path_max=96 atomic_rename=1 sd_touch=0
```

`sd_touch=0` is the important safety contract. The ESP32 exposes this as
`rp2040 ping`, including `public_rf_tx=false` and `formats_sd=false`.

## Diagnostic Request

ESP32 sends this manual, non-formatting probe command when the operator runs
`storage diag`:

```text
DESKOS_SD_DIAG
```

RP2040 replies with one compact line:

```text
DESKOS_SD_DIAG pins=det7-cs13-sck10-mosi11-miso12-pwr18 hz=1000000 pin_sck=1 pin_mosi=1 pin_miso=1 pin_cs=1 selected_power=high selected_mode=dedicated mount_selected=0 detect=floating detect_driven=0 det_pullup=1 det_pulldown=0 hd_p=0 hd_e=254 hd_d=0 hd_c0r=255 hd_c8r=255 hd_c0=255 hd_c8=255 hd_r70=0 hd_r71=0 hd_r72=0 hd_r73=0 hd_miso_pull=1 hd_miso_spi=1 hd_miso_idle=1 hd_idle_ff=255 hd_kb=0 hs_p=0 hs_e=254 hs_d=0 hs_c0r=255 hs_c8r=255 hs_c0=255 hs_c8=255 hs_r70=0 hs_r71=0 hs_r72=0 hs_r73=0 hs_miso_pull=1 hs_miso_spi=1 hs_miso_idle=1 hs_idle_ff=255 hs_kb=0 ld_p=0 ld_e=254 ld_d=0 ld_c0r=255 ld_c8r=255 ld_c0=255 ld_c8=255 ld_r70=0 ld_r71=0 ld_r72=0 ld_r73=0 ld_miso_pull=1 ld_miso_spi=1 ld_miso_idle=1 ld_idle_ff=255 ld_kb=0 ls_p=0 ls_e=254 ls_d=0 ls_c0r=255 ls_c8r=255 ls_c0=255 ls_c8=255 ls_r70=0 ls_r71=0 ls_r72=0 ls_r73=0 ls_miso_pull=1 ls_miso_spi=1 ls_miso_idle=1 ls_idle_ff=255 ls_kb=0 bb_p=0 bb_e=254 bb_d=0 bb_c0r=255 bb_c8r=255 bb_c0=255 bb_c8=255 bb_r70=0 bb_r71=0 bb_r72=0 bb_r73=0 bb_miso_pull=1 bb_miso_spi=1 bb_miso_idle=1 bb_idle_ff=255 bb_kb=0 bi_p=0 bi_e=254 bi_d=0 bi_c0r=255 bi_c8r=255 bi_c0=255 bi_c8=255 bi_r70=0 bi_r71=0 bi_r72=0 bi_r73=0 bi_miso_pull=1 bi_miso_spi=1 bi_miso_idle=1 bi_idle_ff=255 bi_kb=0 bs_p=0 bs_e=254 bs_d=0 bs_c0r=255 bs_c8r=255 bs_c0=255 bs_c8=255 bs_r70=0 bs_r71=0 bs_r72=0 bs_r73=0 bs_miso_pull=1 bs_miso_spi=1 bs_miso_idle=1 bs_idle_ff=255 bs_kb=0 bcm_p=0 bcm_e=254 bcm_d=0 bcm_c0r=255 bcm_c8r=255 bcm_c0=255 bcm_c8=255 bcm_r70=0 bcm_r71=0 bcm_r72=0 bcm_r73=0 bcm_miso_pull=1 bcm_miso_spi=1 bcm_miso_idle=1 bcm_idle_ff=255 bcm_kb=0 bsc_p=0 bsc_e=254 bsc_d=0 bsc_c0r=255 bsc_c8r=255 bsc_c0=255 bsc_c8=255 bsc_r70=0 bsc_r71=0 bsc_r72=0 bsc_r73=0 bsc_miso_pull=1 bsc_miso_spi=1 bsc_miso_idle=1 bsc_idle_ff=255 bsc_kb=0
```

`pin_sck`, `pin_mosi`, and `pin_miso` report whether Arduino-Pico accepted the
configured SPI1 pins; `pin_cs` reports that GPIO13 chip select registration is
configured. `detect`, `detect_driven`, `det_pullup`, and `det_pulldown`
report the raw GPIO7 SD-detect sample. Each probe prefix (`hd`, `hs`, `ld`, `ls`, `bb`, `bi`, `bs`, `bcm`, `bsc`)
reports presence (`*_p`), final probe error (`*_e`), error data (`*_d`), the
CS-low selected-ready byte before CMD0 (`*_c0r`) and before CMD8 (`*_c8r`), raw `CMD0` response
(`*_c0`), raw `CMD8` response (`*_c8`), the four `CMD8` R7 echo bytes
(`*_r70`..`*_r73`), MISO line samples after pull-up, after SPI1 begins, and
after idle clocks (`*_miso_pull`, `*_miso_spi`, `*_miso_idle`), the first
CS-high idle `SPI1.transfer(0xFF)` byte (`*_idle_ff`), and detected capacity in
KiB (`*_kb`). Raw CMD0 probes sample selected-ready and scan past leading
all-zero response bytes inside the response window; the `bb_*` CMD0 path also
retries all-zero CMD0 responses with one to seven pre-command bit clocks while
selected. The `bi_*` path repeats the bit-banged probe with inverted CS
idle/select polarity and never drives MISO. The `bs_*`, `bcm_*`, and `bsc_*`
paths repeat the bit-banged probe with SCK/MOSI, CS/MOSI, and SCK/CS swapped,
exercising only card input lines.
These fields are non-formatting diagnostics for distinguishing a stuck/all-zero
SPI bus, byte-boundary issue, select-polarity mismatch, or safe pin-map mismatch
from a card that reaches SPI idle but fails the SD v2 echo check.

Probe prefixes are `hd` high/dedicated, `hs` high/shared, `ld`
low/dedicated, `ls` low/shared, `bb` high-power bit-banged GPIO probe,
`bi` high-power bit-banged inverted-CS GPIO probe, `bs` high-power
bit-banged SCK/MOSI-swapped GPIO probe, `bcm` high-power bit-banged
CS/MOSI-swapped GPIO probe, and `bsc` high-power bit-banged SCK/CS-swapped
GPIO probe. The `bb`, `bi`, `bs`, `bcm`, and `bsc` probes are
diagnostic-only and do not enable file operations or bypass the ready SD gate.
For each probe, `_p` is present, `_e` is
the SdFat error code, `_d` is error data, and `_kb` is detected capacity. The
command does not format, copy UF2 files, or send RF.

The full raw diagnostic line is intentionally longer than the generic
`DESKOS_SD_FILE` line cap. The ESP32 bridge must capture it with the dedicated
raw-diagnostic buffer while keeping file-operation request and reply lines at
512 bytes.

## Boot Prepare Contract

`d1l_storage_boot_prepare()` runs before retained stores initialize. It may use
safe status and the explicit non-formatting mount path so a ready card can
enable SD-backed retained history before store load. It must stay bounded by the
boot timeout, must not send any formatting command, and must leave onboard NVS
fallback active if the bridge is unavailable, the card is absent, the mount is
still pending, or the ready file-operation gate is not available.

After retained stores initialize, `storage_manager` continues convergence in
the background with explicit states `BRIDGE_WAIT`, `PING`, `STATUS`, `MOUNT`,
`READY_SD`, `READY_NVS`, `NEEDS_FAT32`, `NO_CARD`, and `ERROR_BACKOFF`.
Before any valid DeskOS bridge response is seen, three consecutive initial ping
timeouts queue the existing reset-and-remount recovery exactly once per ESP32
boot. The manager sends one final bounded ping before claiming that queued
reset. Any valid DeskOS ping, status, or mount response cancels that automatic
pulse and permanently disables another for the rest of the boot, so
an active or mounted SD bridge cannot enter a reset storm. The recovery remains
non-formatting and sends no RF.
`storage remount` queues a new non-formatting mount attempt, `storage
reset-bridge` resets the RP2040 bridge and remounts, and `storage force-nvs
[on|off]` provides an operator fallback override without formatting or Public
RF.

## FAT32-Only Storage Policy

There is no RP2040 or ESP32 formatting request. Users must provide a FAT32 SD
card prepared on a computer. When a FAT32 card mounts and `/deskos` is missing,
the bridge creates the required DeskOS folders and manifests. If an existing
DeskOS manifest is invalid, the bridge preserves it as `.bad`, reports
`deskos_manifest_invalid`, and leaves file operations disabled so the ESP32
keeps NVS fallback active with user-facing repair guidance. When no card is
present, the ESP32 uses NVS fallback. When a card is not FAT32 or unmountable,
the ESP32 also keeps NVS fallback active. Do not add developer-only,
serial-only, UI, or script-only format paths.

## Generic File Operations

The file protocol is intentionally generic. Store formats stay above this layer,
so packet logs, map tiles, exports, and later message stores can share the same
bounded byte-file transport.

All file operation requests and replies use the prefix:

```text
DESKOS_SD_FILE v=1 id=<1..65535> op=<operation> ...
```

`id` is generated by the ESP32 and must be echoed by the RP2040. Payloads use
base64url without padding. CRC values are uppercase 8-character CRC32 tokens
over the decoded bytes. Paths are base64url-encoded ASCII relative paths under
`/deskos`: max 96 bytes, `[A-Za-z0-9._/-]` only, no leading slash, no empty
segment, and no `.` or `..` segment.

Supported requests:

```text
DESKOS_SD_FILE v=1 id=7 op=stat path=bG9ncy9wYWNrZXQuYmlu
DESKOS_SD_FILE v=1 id=8 op=read path=bG9ncy9wYWNrZXQuYmlu off=0 len=192
DESKOS_SD_FILE v=1 id=9 op=write path=bG9ncy9wYWNrZXQudG1w off=0 len=5 trunc=1 data=aGVsbG8 crc=3610A686
DESKOS_SD_FILE v=1 id=10 op=append path=bG9ncy9wYWNrZXQudG1w len=17 data=cGFja2V0LWxvZy1jYW5hcnk crc=63072227
DESKOS_SD_FILE v=1 id=11 op=delete path=bG9ncy9wYWNrZXQuYmlu
DESKOS_SD_FILE v=1 id=12 op=rename path=bG9ncy9wYWNrZXQudG1w to=bG9ncy9wYWNrZXQuYmlu replace=1
```

Successful replies:

```text
DESKOS_SD_FILE v=1 id=7 ok=1 op=stat exists=1 kind=file size=22 note=ok
DESKOS_SD_FILE v=1 id=8 ok=1 op=read off=0 len=5 eof=1 data=aGVsbG8 crc=3610A686 note=ok
DESKOS_SD_FILE v=1 id=9 ok=1 op=write off=0 len=5 size=5 note=ok
DESKOS_SD_FILE v=1 id=10 ok=1 op=append off=5 len=17 size=22 note=ok
DESKOS_SD_FILE v=1 id=11 ok=1 op=delete note=ok
DESKOS_SD_FILE v=1 id=12 ok=1 op=rename note=ok
```

Any error reply uses:

```text
DESKOS_SD_FILE v=1 id=<id-or-0> ok=0 op=<op-or-unknown> err=<code> note=<token>
```

Standard error codes are `bad_request`, `bad_value`, `unsupported_op`,
`line_too_long`, `not_ready`, `no_card`, `bad_path`, `not_found`, `is_dir`,
`exists`, `range`, `too_large`, `decode_failed`, `crc_mismatch`, `open_failed`,
`read_failed`, `write_failed`, `flush_failed`, `rename_failed`, `delete_failed`,
`io_error`, and `timeout`.

For `rename replace=1`, the RP2040 first renames an existing final file to an
internal `.bak` path, commits the temp file, then removes the backup. The bridge
must size full-path buffers for the maximum 96-byte relative path plus that
backup suffix so max-length retained-store and map-tile paths can still use
atomic replace safely.

For compacted stores and map tiles, write chunks to a temporary path, then use
`rename replace=1` as the commit step. Retained history blobs use:
`stores/messages/public/public.bin`, `stores/messages/dm/threads.bin`,
`stores/routes/routes.bin`, and `stores/packet_log/ring.bin`, each committed
through a same-directory `.tmp` path. Packet history also keeps a bounded SD
journal in `stores/packet_log/segments/sNN.bin`: 64 segment files
with 64 fixed-size records each, for a 4096-record SD window while NVS keeps the
newest compact fallback rows. Records use sequence-derived fixed offsets; a new
segment cycle starts with `write trunc=1`, and an already matching record is an
idempotent retry. An unwritten exact-EOF slot is the canonical zero-byte reply
`len=0 eof=1 data= crc=00000000`. ESP32 keeps an onboard NVS mirror so
removing or timing out the SD card does not strand message, route, or packet
history. Do not blindly retry `append` after a timeout unless the higher-level
record format has its own idempotency key.

Zero-byte `write` with `trunc=1` is allowed for create/truncate semantics.
Zero-byte `append` is rejected with `bad_value`.

## Storage Filecanary Transcript

The serial `storage filecanary` command uses the generic file protocol under
`/deskos` with these deterministic paths and payload:

- Temp path: `canary/filecanary.tmp`
- Final path: `canary/filecanary.bin`
- Payload: `DeskOS SD file canary v1`

The host simulator prints this exact request/reply shape:

```powershell
python .\tools\rp2040_sd_protocol.py --scenario ready --file-canary-transcript
```

Canonical ready-card transcript with request IDs starting at `1`:

```text
> DESKOS_SD_FILE v=1 id=1 op=delete path=Y2FuYXJ5L2ZpbGVjYW5hcnkudG1w
< DESKOS_SD_FILE v=1 id=1 ok=0 op=delete err=not_found note=not_found
> DESKOS_SD_FILE v=1 id=2 op=delete path=Y2FuYXJ5L2ZpbGVjYW5hcnkuYmlu
< DESKOS_SD_FILE v=1 id=2 ok=0 op=delete err=not_found note=not_found
> DESKOS_SD_FILE v=1 id=3 op=write path=Y2FuYXJ5L2ZpbGVjYW5hcnkudG1w off=0 len=24 trunc=1 data=RGVza09TIFNEIGZpbGUgY2FuYXJ5IHYx crc=471861E4
< DESKOS_SD_FILE v=1 id=3 ok=1 op=write off=0 len=24 size=24 note=ok
> DESKOS_SD_FILE v=1 id=4 op=read path=Y2FuYXJ5L2ZpbGVjYW5hcnkudG1w off=0 len=24
< DESKOS_SD_FILE v=1 id=4 ok=1 op=read off=0 len=24 eof=1 data=RGVza09TIFNEIGZpbGUgY2FuYXJ5IHYx crc=471861E4 note=ok
> DESKOS_SD_FILE v=1 id=5 op=rename path=Y2FuYXJ5L2ZpbGVjYW5hcnkudG1w to=Y2FuYXJ5L2ZpbGVjYW5hcnkuYmlu replace=1
< DESKOS_SD_FILE v=1 id=5 ok=1 op=rename note=ok
> DESKOS_SD_FILE v=1 id=6 op=stat path=Y2FuYXJ5L2ZpbGVjYW5hcnkuYmlu
< DESKOS_SD_FILE v=1 id=6 ok=1 op=stat exists=1 kind=file size=24 note=ok
> DESKOS_SD_FILE v=1 id=7 op=read path=Y2FuYXJ5L2ZpbGVjYW5hcnkuYmlu off=0 len=24
< DESKOS_SD_FILE v=1 id=7 ok=1 op=read off=0 len=24 eof=1 data=RGVza09TIFNEIGZpbGUgY2FuYXJ5IHYx crc=471861E4 note=ok
> DESKOS_SD_FILE v=1 id=8 op=delete path=Y2FuYXJ5L2ZpbGVjYW5hcnkuYmlu
< DESKOS_SD_FILE v=1 id=8 ok=1 op=delete note=ok
> DESKOS_SD_FILE v=1 id=9 op=stat path=Y2FuYXJ5L2ZpbGVjYW5hcnkuYmlu
< DESKOS_SD_FILE v=1 id=9 ok=1 op=stat exists=0 kind=none size=0 note=ok
```

The first two cleanup deletes may return `not_found`; that is acceptable. Live
ESP32 request IDs are process-global, so hardware captures may not start at `1`.

## Storage Export-Canary Transcript

The serial `storage export-canary <token>` command uses the generic file
protocol to prove one diagnostic export commit path. It is not a full export
store migration. With token `export1`, it writes:

- Temp path: `exports/diagnostics/export-canary-export1.tmp`
- Final path: `exports/diagnostics/export-canary-export1.json`
- Payload: `{"schema":1,"kind":"diagnostic_export_canary","token":"export1","public_rf_tx":false,"formats_sd":false}`

The host simulator prints this request/reply shape:

```powershell
python .\tools\rp2040_sd_protocol.py --scenario ready --export-canary-transcript --token export1
```

Canonical ready-card transcript with request IDs starting at `20`:

```text
> DESKOS_SD_FILE v=1 id=20 op=delete path=ZXhwb3J0cy9kaWFnbm9zdGljcy9leHBvcnQtY2FuYXJ5LWV4cG9ydDEudG1w
< DESKOS_SD_FILE v=1 id=20 ok=0 op=delete err=not_found note=not_found
> DESKOS_SD_FILE v=1 id=21 op=write path=ZXhwb3J0cy9kaWFnbm9zdGljcy9leHBvcnQtY2FuYXJ5LWV4cG9ydDEudG1w off=0 len=105 trunc=1 data=eyJzY2hlbWEiOjEsImtpbmQiOiJkaWFnbm9zdGljX2V4cG9ydF9jYW5hcnkiLCJ0b2tlbiI6ImV4cG9ydDEiLCJwdWJsaWNfcmZfdHgiOmZhbHNlLCJmb3JtYXRzX3NkIjpmYWxzZX0K crc=976E78C5
< DESKOS_SD_FILE v=1 id=21 ok=1 op=write off=0 len=105 size=105 note=ok
> DESKOS_SD_FILE v=1 id=22 op=read path=ZXhwb3J0cy9kaWFnbm9zdGljcy9leHBvcnQtY2FuYXJ5LWV4cG9ydDEudG1w off=0 len=105
< DESKOS_SD_FILE v=1 id=22 ok=1 op=read off=0 len=105 eof=1 data=eyJzY2hlbWEiOjEsImtpbmQiOiJkaWFnbm9zdGljX2V4cG9ydF9jYW5hcnkiLCJ0b2tlbiI6ImV4cG9ydDEiLCJwdWJsaWNfcmZfdHgiOmZhbHNlLCJmb3JtYXRzX3NkIjpmYWxzZX0K crc=976E78C5 note=ok
> DESKOS_SD_FILE v=1 id=23 op=rename path=ZXhwb3J0cy9kaWFnbm9zdGljcy9leHBvcnQtY2FuYXJ5LWV4cG9ydDEudG1w to=ZXhwb3J0cy9kaWFnbm9zdGljcy9leHBvcnQtY2FuYXJ5LWV4cG9ydDEuanNvbg replace=1
< DESKOS_SD_FILE v=1 id=23 ok=1 op=rename note=ok
> DESKOS_SD_FILE v=1 id=24 op=stat path=ZXhwb3J0cy9kaWFnbm9zdGljcy9leHBvcnQtY2FuYXJ5LWV4cG9ydDEuanNvbg
< DESKOS_SD_FILE v=1 id=24 ok=1 op=stat exists=1 kind=file size=105 note=ok
> DESKOS_SD_FILE v=1 id=25 op=read path=ZXhwb3J0cy9kaWFnbm9zdGljcy9leHBvcnQtY2FuYXJ5LWV4cG9ydDEuanNvbg off=0 len=105
< DESKOS_SD_FILE v=1 id=25 ok=1 op=read off=0 len=105 eof=1 data=eyJzY2hlbWEiOjEsImtpbmQiOiJkaWFnbm9zdGljX2V4cG9ydF9jYW5hcnkiLCJ0b2tlbiI6ImV4cG9ydDEiLCJwdWJsaWNfcmZfdHgiOmZhbHNlLCJmb3JtYXRzX3NkIjpmYWxzZX0K crc=976E78C5 note=ok
```

The first temp cleanup delete may return `not_found`; that is acceptable. The
final JSON is intentionally left present for inspection.

## Storage Diagnostic-Export Transcript

The serial `storage export-diagnostics <token>` command uses the same file
protocol to commit a bounded diagnostic JSON bundle under:

- Temp path: `exports/diagnostics/diagnostic-export-<token>.tmp`
- Final path: `exports/diagnostics/diagnostic-export-<token>.json`

The bundle includes storage, health, crashlog, limits, and a `map_tiles` object
that reports cache readiness while keeping `exported=false`; diagnostic exports
do not bundle actual tile payloads. It is chunked into 192-byte-or-smaller
writes, then read back from the temp path, committed with `rename replace=1`,
statted, and read back from the final path. The final JSON is intentionally left
present for inspection.

The host simulator prints the deterministic request/reply shape:

```powershell
python .\tools\rp2040_sd_protocol.py --scenario ready --diagnostic-export-transcript --token diag1
```

## Storage Data-Export Transcript

The serial `storage export-data <token>` command uses the same file protocol to
commit a bounded sampled user-data JSON bundle under:

- Temp path: `exports/data/data-export-<token>.tmp`
- Final path: `exports/data/data-export-<token>.json`

The bundle includes storage backend labels, a settings summary, recent Public
messages, DMs, routes, packets, contacts, nodes, and read-state counters. It
sets `private_identity_exported=false` and does not serialize the private
identity key. It is chunked into 192-byte-or-smaller writes, read back from the
temp path, committed with `rename replace=1`, statted, and read back from the
final path. The final JSON is intentionally left present for inspection.

The host simulator prints the deterministic request/reply shape:

```powershell
python .\tools\rp2040_sd_protocol.py --scenario ready --data-export-transcript --token data1
```

## Map Tile Cache Canary Transcript

The serial `storage map-tile-canary <token>` command uses the same file protocol
to commit a synthetic tile under:

- Temp path: `map/tiles/z12/x1/y2-<token>.tmp`
- Final path: `map/tiles/z12/x1/y2-<token>.tile`

The canary writes a small JSON payload, reads it back from the temp path,
commits with `rename replace=1`, stats the final path, and reads the final tile
back. The final synthetic tile is intentionally left present for inspection.
This proves the nested cache directory and atomic commit path without Public RF
or formatting.

The serial `storage map-policy` command is read-only. It reports the built-in
current-view Map policy, the production cache path template
`map/tiles/z{z}/x{x}/y{y}.tile`, the current `map_tile_backend`, and
`download_supported=false`/`live_network_download=false`. It does not request
network tiles, send Public RF, or format the card.

The host simulator prints the deterministic request/reply shape:

```powershell
python .\tools\rp2040_sd_protocol.py --scenario ready --map-tile-canary-transcript --token map1
```

## Safety Rules

- No on-device SD formatting from boot, UI, serial, RP2040 firmware, ESP32 firmware, or scripts.
- Users must supply FAT32 SD cards prepared on a computer.
- Settings, identity, and minimum boot-critical state remain on onboard NVS.
- Until SD-backed retained-history stores are enabled, valid SD cards are reported as `store_migration_pending`.
- The RP2040 bridge may create `/deskos` on a mounted card and exposes bounded file operations. Public/DM message history, route history, and packet history can use SD when ready with NVS mirrors; retained-store SD write/read/rename failures are counted separately from NVS mirror failures, and the UI must show `SD degraded; using internal fallback` after a real SD failure latches. Diagnostic and sampled-data exports use their existing bounded paths. `storage map-tile-canary` is a synthetic storage-only check: it never performs network I/O and is not live-map acceptance.
- OpenStreetMap Standard remains built-in, attribution-bearing, and strictly current-view-only: it may fetch only the visible 3x3 view at one zoom, and it is never accepted for background or area prefetch. A separately supplied DeskOS-relative `map/offline-provider.json` may enable HTTPS offline storage and background prefetch only when its manifest explicitly grants both permissions and supplies attribution and a license URL. With configured device location, working Wi-Fi, and prepared SD storage, that authorized provider may cache the device area plus GPS node adverts within 200 km at the highest zoom that fits the fixed card allocation and 8 GiB reserve. `map acceptance status` reports the machine evidence. `map acceptance open` accepts no URL or coordinates and forces a new Map generation and SD reread, so the offline acceptance revisit cannot pass by reusing a retained rendered frame; it must complete from cached tiles with zero view network requests.
