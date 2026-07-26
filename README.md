# MeshCore DeskOS for SenseCAP Indicator D1L

MeshCore DeskOS is a touch-first, non-forwarding MeshCore desk client for the
Seeed SenseCAP Indicator D1L. The production 1.0 / RC1 candidate builds the
`core_1_0` profile with conditional SD history:

```text
D1L_RELEASE_PROFILE=core_1_0
D1L_SD_HISTORY_MODE=conditional
```

The agreed RC1 firmware surface is implemented in source. Public release
remains fail-closed until the exact commit is built by GitHub Actions, the
downloaded package and provenance are checksum-verified, that exact artifact
is flashed to the qualified D1L, and the bounded device acceptance receipt
passes. Source tests, simulator images, predecessor binaries, local firmware
builds, and dry runs are not release evidence.

The current D1L is attached to Raspberry Pi 5 host `neopi5`
(`192.168.0.24`). Its only authorized release identity is:

```text
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
VID:PID 1a86:7523
```

Do not substitute a raw `/dev/ttyUSB*` name or a stale Windows COM assignment.

## Production feature set

| Area | DeskOS 1.0 / RC1 production surface |
|---|---|
| Hardware | 480×480 display, touch, button, SX1262 radio, backlight and power/status truth |
| Messaging | Public and multi-channel messaging, DMs, unread state, delivery/retry truth and retained history |
| Contacts and nodes | Verified contacts, USB MeshCore URI import, rename/favorite/mute/delete, up to 512 retained heard nodes, role detail and signed advert-location markers |
| Network tools | Finder, repeater Ping, contact PATH/TRACE, packet terminal, filters/search/raw detail, signal and route evidence |
| Map | Built-in attributed OpenStreetMap visible-view cache plus authorized-provider background prefetch around the device and signed nodes within 200 km |
| Connectivity | User-controlled multiple saved Wi-Fi profiles; BLE companion transport is deferred to RC2 |
| Server administration | Authenticated repeater/room login, status, telemetry, neighbours, ACL, bounded CLI, room posts, logout and locally confirmed mutations |
| Observer | Opt-in `mqtts://` TLS observer, QoS 1/PUBACK accounting, bounded queue and optional center location |
| Storage | Externally prepared FAT32 SD/RP2040 primary history, exports and map cache, with visible degraded operation when SD is unavailable |
| Deferred to 1.5 / RC2 | BLE companion transport, contact/channel QR sharing, and signed OTA/update/recovery product workflows |
| Device UX | Brightness, timeout, night/high-contrast modes, notification pulse/quiet modes, curated glyph palette and service sheets |
| Support | Structured event terminal, diagnostics, crashlog, health, guarded reboot/factory reset and normal non-erasing USB flashing |

The D1L has no onboard GPS. Map centers on an explicitly configured device
location. Peer pins require signed advert coordinates and truthful time/age
validation.

Built-in OpenStreetMap Standard access is visible-current-view-only: one 3×3
tile plan at one zoom per visible generation, with attribution and completed
tile reuse from SD. Background/offline download is available only through an
installed HTTPS provider manifest that explicitly authorizes offline storage
and background prefetch. Background prefetch pauses while the interactive Map
is open.

## Navigation

The RC1 dock contains:

1. Home
2. Messages
3. Nodes
4. Map
5. Tools

Tools groups Packets, Diagnostics, Terminal, Wi-Fi, Observer, SD Card, Map
options, Display, Notifications, Identity, About, Radio, Server admin, and
Mesh advertise. Lists and detail sheets scroll on the device.

## Safety and privacy

- DeskOS is a client and does not forward MeshCore packets.
- Read, navigation, search, refresh and automated UI acceptance are RF-silent.
- Release automation transmits on the default Public channel only for the
  single tokenized RC1 hardware gate when the operator explicitly supplies
  `--authorize-public-tx`; all other automated validation remains Public-silent.
- Observer is off until configured and enabled; it never publishes message
  text, contacts, keys, or forwarding traffic.
- Server mutations require the exact authenticated target/session and a second
  local confirmation. Sensitive input is not persistently retained or logged;
  volatile confirmation buffers are wiped.
- Users prepare FAT32 SD cards on a computer. There is no device-side SD
  formatting path. Missing or unusable media enters a visible live-only RF
  chat mode; retained history is not silently redirected into default NVS.
- Default NVS remains a bounded configuration, identity, boot/recovery and
  diagnostic store, not the retained-history authority.
- Normal project flashing is non-erasing. OTA/update/recovery product
  workflows are deferred to 1.5 / RC2.

Prepare an already-formatted 32GB-or-larger FAT32 card without formatting,
deleting, or overwriting files:

```powershell
python .\scripts\prepare_deskos_sd.py --target E:\ --apply
```

The checked-in payload is under `sdcard/deskos`. Run the command without
`--apply` for a read-only plan. Optional preloaded tiles require an explicit
provider manifest granting offline storage; the public OpenStreetMap Standard
tile service is interactive-cache only and must not be bulk downloaded.

The provider manifest is optional for a normal card. It is required only for
authorized background/offline Map downloads.

## Build and release policy

Release firmware is built only by `.github/workflows/d1l-ci.yml` with the
pinned ESP-IDF 5.5.4 toolchain. Local workstations may run source/host tests
but do not produce release firmware.

During development, run only the focused tests for the changed slice and the
relevant generated-document/package checks. The exact GitHub Actions run owns
the complete release build and gate:

```powershell
python -m pytest <focused-test-files> -q
git diff --check
```

The exact Actions run must produce a source-bound `core_1_0` package,
checksums, provenance, SBOM, MeshCore conformance evidence, and the
conditional-SD RP2040 bridge payload. Package metadata records the exact
repository, commit, workflow run, run attempt, release profile and SD mode.
Only this downloaded and verified package may be used for final hardware
acceptance.

## Flash the current device

Extract the exact Actions package on `neopi5`, verify it, then use only the
stable by-id link:

```bash
cd /path/to/extracted-package
sha256sum --check SHA256SUMS.txt
export D1L_PORT='/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
test -L "$D1L_PORT" && test -r "$D1L_PORT" && test -w "$D1L_PORT"
D1L_DEVICE_PROPERTIES="$(udevadm info --query=property --name="$D1L_PORT")"
grep -qx 'ID_VENDOR_ID=1a86' <<<"$D1L_DEVICE_PROPERTIES"
grep -qx 'ID_MODEL_ID=7523' <<<"$D1L_DEVICE_PROPERTIES"
./flash_project.sh
```

Follow the package README for its exact file names and acceptance command.
Never flash another Pi serial device. Never format SD.

That package helper is for an ordinary post-release install. It is not the
closing path for `v1.0.0`; the production candidate must follow the
[authoritative RC1 release execution](docs/RC1_RELEASE_EXECUTION_D1L.md),
including its bootstrap and retained-reflash receipts, bounded eight-source
gate, final audit, exact tag and checksummed release assets.

## Documentation

- [DeskOS 1.0 / RC1 user guide](docs/USER_GUIDE_D1L.md)
- [Authoritative RC1 release execution](docs/RC1_RELEASE_EXECUTION_D1L.md)
- [Current RC1 product contract](docs/release/SIGUI_CORE_1_0_PRODUCT_CONTRACT_2026-07-18.md)
- [Install/recovery history](docs/FLASH_RECOVERY_D1L.md)
- [Historical 24-hour status ledger](docs/release/24H_STATUS.md)
- [Known limitations and security boundaries](docs/KNOWN_LIMITATIONS.md)
- [Acceptance plan](docs/TEST_PLAN_D1L.md)
- [MeshCore conformance](docs/MESHCORE_CONFORMANCE.md)
- [Attribution](docs/ATTRIBUTIONS.md)

## Licensing

MeshCore DeskOS D1L is GPL-3.0-or-later. Release packages include third-party
notices and source attribution for pinned dependencies and permitted
references.
