# MeshCore DeskOS for SenseCAP Indicator D1L

MeshCore DeskOS is a touch-first, non-forwarding MeshCore desk client for the
Seeed SenseCAP Indicator D1L. The production candidate now builds the
`full_feature` profile with conditional SD history:

```text
D1L_RELEASE_PROFILE=full_feature
D1L_SD_HISTORY_MODE=conditional
```

The firmware feature implementation is complete for this profile. Public
release remains fail-closed until the exact commit built by GitHub Actions is
checksum-verified, flashed to the qualified D1L, and its automated device,
reboot, storage, and controlled-peer acceptance receipts pass. Source tests,
simulator images, predecessor binaries, and dry runs are not release evidence.

The current D1L is attached to Raspberry Pi 5 host `neopi5`
(`192.168.0.24`). Its only authorized release identity is:

```text
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
VID:PID 1a86:7523
```

Do not substitute a raw `/dev/ttyUSB*` name or a stale Windows COM assignment.

## Production feature set

| Area | Full Feature production surface |
|---|---|
| Hardware | 480×480 display, touch, button, SX1262 radio, backlight and power/status truth |
| Messaging | Public and multi-channel messaging, DMs, unread state, delivery/retry truth and retained history |
| Contacts and nodes | Verified contacts, QR import/export, rename/favorite/mute/delete, heard nodes, role detail and signed location markers |
| Network tools | Packet terminal, filters/search/raw detail, signal/routes and explicit user TRACE/PATH tools |
| Map | Built-in attributed OpenStreetMap source, manual or authenticated-companion center, visible-current-view tile cache and signed peer markers |
| Connectivity | User-controlled Wi-Fi and bonded encrypted BLE companion transport |
| Companion protocol | Official core initial-sync, contact/channel, messaging, time, advert, radio and battery/storage commands |
| Server administration | Authenticated repeater and room login/status; room login starts with a no-history cursor; two exact allowlisted mutations require local confirmation |
| Observer | Opt-in `mqtts://` TLS observer, QoS 1/PUBACK accounting, bounded queue and optional center location |
| Storage | Externally prepared FAT32 SD/RP2040 primary history, exports and map cache, with visible degraded operation when SD is unavailable |
| Updates | Ed25519-signed local SD/OTA bundle, exact signer identity, image hash, anti-rollback sequence, dual-slot boot and rollback |
| Device UX | Brightness, timeout, night/high-contrast modes, notification pulse/quiet modes, curated glyph palette and service sheets |
| Support | Structured event terminal, diagnostics, crashlog, health, safe reboot, guarded factory reset and USB recovery |

The D1L has no onboard GPS. A map center comes only from an explicit local
entry or an authenticated bonded companion. Peer pins require signed advert
coordinates and truthful time/age validation.

The BLE companion surface intentionally rejects remote reboot, factory reset,
and private-key import/export. Optional channel-datagram extensions are not
advertised. Those exclusions protect device ownership and secrets; normal
companion setup, synchronization, channel/contact management, messaging,
location, advert and radio operations are available.

## Navigation

The full-feature dock contains:

1. Home
2. Messages
3. Nodes
4. Map
5. Tools

Tools groups Packets, Diagnostics, Terminal, Wi-Fi, Bluetooth, Observer, SD
Card, Map options, Signed update, Display, Notifications, Identity, About,
Radio, Server admin, and Mesh advertise. Lists and detail sheets scroll on the
device; the automated UI probe covers required scrollable surfaces.

## Safety and privacy

- DeskOS is a client and does not forward MeshCore packets.
- Read, navigation, search, refresh and automated UI acceptance are RF-silent.
- Release automation never transmits on the default Public channel.
- Observer is off until configured and enabled; it never publishes message
  text, contacts, keys, or forwarding traffic.
- Server mutations are limited to `clear stats` and `advert.zerohop`, require
  an authenticated session, and require a second local confirmation within
  five seconds.
- Users prepare FAT32 SD cards on a computer. There is no device-side SD
  formatting path. Missing or unusable media falls back to NVS where defined.
- Normal project flashing is non-erasing. The full recovery image is
  destructive and requires typed confirmation.

Prepare an already-formatted 32GB-or-larger FAT32 card without formatting,
deleting, or overwriting files:

```powershell
python .\scripts\prepare_deskos_sd.py --target E:\ --apply
```

The checked-in payload is under `sdcard/deskos`. Run the command without
`--apply` for a read-only plan. Optional preloaded tiles require an explicit
provider manifest granting offline storage; the public OpenStreetMap Standard
tile service is interactive-cache only and must not be bulk downloaded.

The compatibility `core_1_0` profile remains in source for narrow recovery
builds. In that profile SD history is disabled, NVS is authoritative, and the
RP2040 payload is omitted; it is not the current production candidate.

## Build and release policy

Release firmware is built only by `.github/workflows/d1l-ci.yml` with the
pinned ESP-IDF 5.5.4 toolchain. Local workstations may run source/host tests
but do not produce release firmware.

The required source gate is:

```powershell
python -m pytest tests -q
python .\scripts\completion_ledger.py validate --check-generated
python .\scripts\completion_pack_manifest.py check
git diff --check
```

The exact Actions run must produce a source-bound full-feature package,
checksums, provenance, SBOM, MeshCore conformance evidence, RP2040 bridge
payload, and Ed25519-signed update bundle. Package metadata records the exact
repository, commit, workflow run, run attempt, release profile, SD mode and
security sequence.

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

## Documentation

- [Full Feature user guide](docs/USER_GUIDE_D1L.md)
- [Flash and recovery](docs/FLASH_RECOVERY_D1L.md)
- [Current release status](docs/release/24H_STATUS.md)
- [Known limitations and security boundaries](docs/KNOWN_LIMITATIONS.md)
- [Acceptance plan](docs/TEST_PLAN_D1L.md)
- [MeshCore conformance](docs/MESHCORE_CONFORMANCE.md)
- [Attribution](docs/ATTRIBUTIONS.md)

## Licensing

MeshCore DeskOS D1L is GPL-3.0-or-later. Release packages include third-party
notices and source attribution for pinned dependencies and permitted
references.
