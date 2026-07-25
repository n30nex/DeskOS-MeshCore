# MeshCore DeskOS D1L Full Feature User Guide

This guide covers the production `full_feature` firmware for the Seeed
SenseCAP Indicator D1L. DeskOS is a non-forwarding MeshCore desk client: it
receives and sends user-requested traffic but does not act as a repeater.

## First start

1. Power on the D1L and complete onboarding.
2. Set a device name and confirm the Canada/USA radio profile unless your
   authorized region requires another supported setting.
3. Open **Tools → Identity** and confirm the local identity is ready.
4. Open **Tools → Diagnostics** and confirm board, radio, settings and retained
   storage are healthy.
5. Leave Wi-Fi, Bluetooth and Observer off until you need them.

The bottom dock is **Home**, **Messages**, **Nodes**, **Map**, and **Tools**.
Swipe vertically in lists and long sheets. Home itself is a summary rather
than a long scrolling page.

## Messages and channels

**Messages** opens the channel and direct-message views. Reading, scrolling,
searching and refreshing are RF-silent. A transmission occurs only after an
explicit Send, Advert, Trace or administration action.

The fixed Public channel is always present. Full Feature also supports
creating/importing channels, selecting an active channel, renaming compatible
entries, exporting share URIs, and removing non-protected channels. The Public
channel cannot be deleted.

A Public packet's display name has the `sender_name_unverified` boundary. It
never alias-matches into a DM destination. Direct-message compose requires the
complete public key of the same retained, verified chat contact; heard-only,
incomplete, mismatched and non-chat identities remain read-only.

DM rows report queued, transmitted, acknowledged, retrying or failed state.
Opening a thread or refreshing it never silently retries a failed draft.

## Contacts, nodes, routes and packets

Contact detail supports verified MeshCore QR import/export, rename,
favorite/mute state and confirmed deletion. Unknown or malformed roles remain
read-only.

**Nodes** shows heard peers and role-specific detail. Signed advert locations
can appear as map markers only when their provenance and non-future age can be
validated. Route and signal views expose retained evidence without inventing
hop identity.

**Tools → Packets** is a bounded terminal-style packet view with pause/resume,
filters, search, packet detail and raw preview. **Trace** and path probes are
explicit RF actions; ordinary inspection is silent.

## Map

The D1L has no onboard GPS. The GPS/location boundary is explicit: Map center is supplied by an explicit user-set
center or an authenticated bonded companion. The source is stored and shown
as `manual` or `authenticated_companion`; it is never inferred from the
coordinates.

Use **Map → Map options → Set location or Cache status**. The map uses the
built-in OpenStreetMap tile source and always displays
`(c) OpenStreetMap contributors`. The bounded interactive policy is:

- zoom 8 through 14, starting at 10;
- one-finger pan and direct `-`, `+`, and **Center** controls;
- a visible current-view 3x3 tile maximum;
- at most one zoom request per visible generation;
- cache/reuse of completed tiles;
- no background prefetch and no area download.

Map probes never request map tiles. Tile networking requires enabled,
connected Wi-Fi, a trusted center, and ready SD-backed tile storage.

## Wi-Fi

Open **Tools → Connections → Wi-Fi**. Scan, save a station profile, connect or
disconnect explicitly. Saved passwords are not printed by status, logs or
exports. Wi-Fi and BLE share the device radio under an offline-first
coexistence policy, so enabling one may stop the other.

## Bluetooth companion

Open **Tools → Connections → Bluetooth**, turn BLE on, tap **Pair**, and enter
the six-digit PIN shown on the D1L in the companion app. A session is accepted
only after encryption, authentication and bonding. **Forget** removes the
bonded peer locally.

The production companion core protocol supports:

- app start and device query;
- contact list and exact-contact lookup/removal;
- channel read/write and non-protected channel deletion;
- DM and channel messaging plus message synchronization;
- device time read/write;
- device name, advert and authenticated-companion location;
- radio parameters, TX power and path-hash mode;
- battery and storage status.

Remote reboot, remote factory reset, and private-key import/export return a
disabled response. Optional channel-datagram extensions are not advertised.
These are deliberate ownership/security boundaries, not missing everyday
companion functions.

## Observer / MQTT

Observer is opt-in. Configure it over the USB terminal with an `mqtts://`
broker and a topic, then enable it from **Tools → Connections → Observer** or
USB. TLS is mandatory. Publishing uses QoS 1 with PUBACK accounting and a
bounded drop-oldest queue during outages.

The payload contains device health/counters and, only when explicitly
selected, the current manual/companion map center. It never publishes message
text, keys, contacts or RF-forwarding data.

## Repeater and room administration

Open a repeater or room node, then **Admin**, or use **Tools → Advanced →
Server admin**. Login requires the exact retained server fingerprint and its
password. A room login starts at the no-history cursor, so old room traffic is
not replayed into the session.

Status refresh is read-only. Production mutations are exactly:

- clear server statistics;
- request a zero-hop advert.

Each requires an authenticated matching session and a second local tap within
five seconds. Logout clears the session. DeskOS does not expose arbitrary raw
server commands.

## Display, notifications and terminal

**Tools → Device → Display** controls brightness, screen timeout, night mode,
high contrast and fixed UTC display offset. Touch wakes the backlight without
also activating the control beneath it.

**Notifications** cycles off, pulse and quiet-hours behavior. Repeated updates
are deduplicated and the backlight pulse does not override an active local
interaction.

**Terminal** shows the 64-entry structured event ring with level, source, kind
and bounded message. It never retains secrets or raw remote commands. Log
clearing is locally confirmed.

## SD storage

Full Feature uses conditional SD history through the D1L RP2040 bridge.
Internal NVS remains the bounded fallback when the card or bridge is absent.

- Users prepare FAT32 SD cards on a computer.
- There is no device-side SD formatting path.
- Non-FAT32, unmountable or foreign-lineage media are preserved and reported
  without destructive repair.
- Retained Public/DM/route/packet data and map/export data use the SD path only
  when ownership and filesystem checks pass.
- Never remove a card while a write is active; use the status page before
  troubleshooting.

The compatibility Core profile has SD history disabled, uses NVS, and omits
the RP2040 payload. It is not the current production candidate.

## Signed update

The release package contains `d1l-update.bin`, `d1l-update.manifest`, and
`d1l-update.sig`. Copy them to the package-documented `updates` directory on
qualified storage, then open **Tools → Storage & maps → Signed update**.

DeskOS verifies the production Ed25519 signer, manifest, exact image SHA-256,
image size, source commit and a strictly increasing security sequence before
writing the inactive OTA slot. Installation and reboot each require local
confirmation. The bootloader can roll back an unconfirmed image; a healthy
boot confirms the running slot. A lower or equal security sequence is
rejected.

USB equivalents are:

```text
update status
update install CONFIRM-SIGNED-UPDATE
update cancel
update reboot CONFIRM-REBOOT-UPDATE
```

## Useful USB diagnostics

The console emits bounded JSON. Start with:

```text
version
health
board
settings get
mesh status
radio get
identity status
storage status
messages unread
nodes
packets
signal
ble status
observer status
admin status
terminal status
update status
crashlog
```

`help` lists the complete allowlist. Commands that transmit, mutate remote
state, clear evidence, reboot, update or factory-reset have explicit
confirmation and release-profile gates.

## Install and recovery

Release firmware is built only by GitHub Actions. Use the exact source-bound
package and verify `SHA256SUMS.txt` before flashing. The current target is on
Pi 5 host `neopi5` and must be selected only by:

```text
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
VID:PID 1a86:7523
```

Never substitute a raw `/dev/ttyUSB*` path or a stale Windows COM assignment.
Normal project flashing is non-erasing. The full 8 MB recovery image can erase
settings, identity, contacts, messages and logs and requires typed
confirmation. See `FLASH_RECOVERY_D1L.md`.

## Release evidence

Firmware completeness and public-release authorization are different gates.
The feature surface is implemented, but a release is authorized only after
the exact Actions artifact, checksum/provenance/SBOM checks, exact-target
flash, automated device acceptance, reboot/persistence, conditional SD,
controlled-peer RF/DM/admin and final physical UI review all pass. Until then
`full_feature_release_ready` remains false.
