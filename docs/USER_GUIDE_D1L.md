# MeshCore DeskOS D1L 1.7.6 User Guide

This guide covers the production `full_feature` firmware with `conditional` SD
history for the Seeed SenseCAP Indicator D1L. DeskOS is a non-forwarding
MeshCore client: it sends and receives user-requested traffic but does not
repeat other devices' traffic.

DeskOS 1.7.6 includes secure BLE companion access, public-data QR sharing,
signed local updates with rollback, touch-first repeater management, and the
guided bridge and SD installation path. The remaining intentional limits and
D1L adaptations are in
[`KNOWN_LIMITATIONS.md`](KNOWN_LIMITATIONS.md) and
[`DESKOS_MESHCORE_FEATURE_PARITY.md`](DESKOS_MESHCORE_FEATURE_PARITY.md).

## First start

Every boot begins with a full-screen readiness check. It shows live progress
for **Display**, **Identity**, **Radio**, **Storage & maps**, and **UI** and
does not uncover Home until all five essential rows are green. Prepared
SD-card and NRCan map-provider status is also shown explicitly: missing or
unprepared media is reported as not ready, never as a successful check.

An already-configured device proceeds to Home without changing its retained
settings. A factory-fresh device opens the first-start wizard:

1. Enter an explicit device name. The field starts blank, and setup cannot
   invent or accept the factory placeholder as the completed name.
2. Optionally enter manual decimal latitude and longitude, or choose **Skip**.
   DeskOS contains no baked-in location.
3. Optionally enter a Wi-Fi network and masked password, or choose **Skip** for
   offline use. The password field is wiped when the page is left.
4. Confirm the fixed Canadian production preset:
   **910.525 MHz, BW 62.5 kHz, SF7, CR5**.
5. Verify the required SD and map readiness. Prepare the FAT32 card on a
   computer; DeskOS firmware never formats cards. The prepared-card workflow
   installs the authorized NRCan manifest at `map/offline-provider.json`.
   **Continue** unlocks only when both are ready.
6. Review the initial **Public**, **#bot**, and **#test** channels, then choose
   **Finish setup**.

Wi-Fi remains optional for offline MeshCore use. The prepared FAT32 card and
NRCan provider manifest are required for the complete 1.5 setup.

The dock is **Home**, **Channels**, **Contacts**, **Map**, and **Settings**.
Home is a summary page; lists and long pages scroll vertically.

A factory-fresh DeskOS does not advertise before onboarding is complete. When
the entered name is saved, DeskOS starts MeshCore receive and queues the first
signed flood advert for that retained identity. Later boots queue that advert
only for an already-onboarded device. This lets nearby clients attribute later
Public messages instead of showing only `Unknown`.

On first setup, DeskOS adds the standard interoperable `#bot` and `#test`
channels while keeping Public selected. Retained configured devices are not
reseeded.

## Messages, channels and direct messages

The Public channel is always configured. Open **Channels** and tap Public or
any other enabled channel to select it and immediately open its conversation.
Use the channel controls to create or import, select, enable, rename, make
default, and remove with local confirmation. A selected channel can display a
one-time QR containing only its supported public import URI.

Public and channel views support send, receive, retained history, search and
unread state. Public display names have the `sender_name_unverified` boundary.
A displayed name never alias-matches into a direct-message destination.
Direct-message compose requires the
complete public key of a retained verified chat contact. Heard-only, truncated,
mismatched and non-chat identities remain read-only.

DM rows report queued, transmitted, acknowledged, retrying or failed state.
Opening or refreshing a thread does not silently retry a failed message.

## Contacts, Finder, Ping and TRACE

Contacts can be imported from the USB console with
`contacts import <meshcore-uri>`. The touchscreen list shows every saved
contact. Use **Search** to match name, role, fingerprint, or public key, and use
**Sort** to cycle **Recent**, **A-Z**, **Role**, and **Signal**. Selecting a row
shows direct **Message** and **Manage** actions alongside rename, favorite,
mute, and confirmed removal.

**Message** opens the existing DM composer/thread for a verified Chat or
Companion contact. **Manage** opens node detail for a Repeater or Room and
provides its status/login workflow even when the contact has no current
heard-node row. **Export QR** displays only the selected contact's supported
public MeshCore URI and clears the temporary payload after rendering.

The same Contacts area provides:

- **Find** sends a zero-hop discovery request and lists returned full keys,
  role and there/back SNR. Finder results are unverified until a signed advert
  is received and therefore are not automatically promoted to contacts.
- **Ping** on a repeater sends a direct zero-hop TRACE.
- **PATH/TRACE** on a verified contact displays pending, timeout, reply, RTT,
  RSSI and hop SNR state.
- **Clear** requires confirmation and removes the retained heard-node list.

Ordinary inspection, scrolling, filtering and refresh are RF-silent.
The retained list is bounded to 512 nodes. At capacity, only an unlocated
least-recent entry may be replaced. A node with a valid signed-advert location
stays on Map until you explicitly clear the node list; if all 512 entries are
located, a new fingerprint is rejected rather than evicting a marker.

## Map

The D1L has no onboard GPS. The GPS/location boundary is explicit: set the
device location from the Map location workflow. DeskOS centers on that
configured location and plots only valid signed peer-advert coordinates.
Markers follow the bounded retained-node list and update when newer signed
data replaces an advert.

Interactive Map supports one-finger pan, **-**, **+**, and **Center** from zoom
8 through 18, limited by the selected provider.
Completed tiles are reused from SD.

The built-in OpenStreetMap Standard source displays
`(c) OpenStreetMap contributors` and fetches only the visible current-view 3×3
at one zoom per visible generation while Map is open. Read-only Map probes
never request tiles. Authorized-provider background prefetch pauses while the
interactive Map is open and resumes after it closes.

Automatic download starts in the background only when all of these are true:

- a device location is configured;
- Wi-Fi is enabled and connected;
- SD-backed Map storage is ready;
- the installed provider manifest explicitly permits offline storage and
  background prefetch.

The download bounds include the device location and signed nodes no farther
than 200 km, plus a small edge margin. DeskOS selects the highest zoom from
8–18 that fits the provider and card budget. Map data may use at most 60% of
the card and must leave at least 8 GiB outside the Map allocation. A provider
without explicit prefetch permission remains visible-view-only.

Provider attribution is shown on-device and must remain with distributed map
data. Do not configure a service whose license or tile policy forbids offline
storage or bulk/background retrieval.

## Wi-Fi

Open **Settings → Connections → Wi-Fi** to scan, save profiles, select one,
delete one, connect, disconnect or re-enable automatic reconnect. Passwords
are never printed by status, logs or exports.

Mesh messaging does not require Wi-Fi. If Wi-Fi is unavailable, cached Map
tiles remain usable and RF chat continues.

## Local clock

Open **Settings -> Display** and use **Time -1h** or **Time +1h** to adjust the
displayed local clock in one-hour steps. The selected fixed UTC offset is saved
across restarts. Mountain Time uses UTC-7 in standard time and UTC-6 in
daylight time.

DeskOS does not currently apply daylight-saving changes automatically. Adjust
the offset when the clock changes. This setting changes only human-readable
display time; radio, security, ordering, and retained protocol timestamps stay
in UTC.

## BLE companion

Open **Settings -> Connections -> BLE**, enable BLE, then choose **Pair**. The
screen shows the current advertising, pairing, security, protocol, and failure
state. Confirm the displayed six-digit passkey in the companion client. Use
**Forget** to remove the retained bond before changing owners or clients.

Transport does not become ready until the connection is encrypted,
authenticated, bonded, and subscribed to notifications. The implementation
uses the official MeshCore service/RX/TX UUIDs and presents the existing
three-byte companion protocol to the single-owner MeshCore runtime. Enabling
BLE and Wi-Fi is mutually exclusive so both network stacks cannot silently
compete for ownership and memory.

The BLE client can use normal supported messaging, contact, channel, time,
radio, advert, and status operations. Private-key import/export, remote reboot,
and factory reset fail closed over BLE. Pairing secrets and private material
are not written to status, logs, screenshots, or exports.

## Signed local update and rollback

Each trusted 1.5 release package includes one matching set under `update/`:

```text
d1l-update.manifest
d1l-update.sig
d1l-update.bin
```

Copy all three files from the same release to `updates/` on the prepared SD
card. Open **Settings -> Signed Update**, choose **Install from SD**, and tap
the second confirmation within five seconds. DeskOS verifies the product,
target, partition-table hash, image size/hash, signer, Ed25519 signature, and
anti-downgrade security sequence before writing the inactive slot. It never
accepts an RF-triggered update.

When the write completes, choose **Reboot to Update** and confirm it. The new
image starts in pending-verification state. A successful normal boot confirms
it; a failed boot rolls back to the previous working slot. The published USB
app BIN and full-clean 8 MB BIN remain the recovery paths if local update is
unavailable.

## Repeater and room administration

The administration capability uses an exact verified Repeater or Room key and
a masked password. Choose **Login** beside a saved repeater or room contact, or
open its detail and choose **Login**. DeskOS opens a large password field and
on-screen keyboard. **Save: On** remembers a non-empty password for that server
on this D1L after login succeeds; **Forget saved** removes it. Blank-password
negotiation remains available when supported by the peer.

After authentication, DeskOS opens a command dashboard rather than returning
to the contact list. The server reports the session permissions (guest,
read-only, write or admin). Logging out or switching targets clears volatile
session authority.

The on-device admin surface provides:

- login, logout, session state and selected route;
- a compact icon dashboard with focused management pages;
- detailed status/counters and telemetry;
- paged neighbours;
- ACL query and confirmed ACL mutations;
- bounded authenticated CLI request/reply;
- device, radio and advert queries/mutations with local confirmation;
- room posts and current-session transcript.

Every request opens a visible animated working state and then leaves its result
on the relevant page. Guest/admin role checks are enforced locally and by the
remote response. Passwords are never logged or exported. Unsaved and temporary
password input is wiped; saved passwords are device-local and are removed when
the contact is forgotten or the device is factory-reset. Sensitive replies are
redacted and volatile confirmation input is wiped. Remote mutations require a
second local confirmation.
The exact role-aware command surface and USB wrapper syntax are listed in
[ADMIN_REMOTE_CLI_ALLOWLIST.md](ADMIN_REMOTE_CLI_ALLOWLIST.md). Commands
outside that list fail closed.

## SD-first storage and degraded mode

DeskOS uses SD as the primary store for message history, contacts/nodes,
routes, packets, Map tiles and exports. Default NVS holds bounded device,
configuration, boot/recovery and diagnostic metadata, including identity,
Wi-Fi, channel definitions, Observer configuration, display/time, crash and
reset state. Retained history is not redirected there.

- The firmware never formats an SD card.
- Prepare a 32GB-or-larger FAT32 card with the browser flasher or checked-in
  `scripts/prepare_deskos_sd.py` workflow. The first-start flow requires
  the authorized NRCan provider manifest for background/offline Map download.
- Foreign, non-FAT32 or unmountable media is preserved and reported.
- A missing/unusable card activates a prominent degraded notice.
- Degraded mode keeps basic live RF Public/channel/DM chat available in a
  live-only session, but retained history, Map download/cache and exports are
  unavailable.
- DeskOS does not silently redirect history into default NVS.

Use **Settings → Storage** before removing media or diagnosing a card.

## Diagnostics and Observer

**Settings → Diagnostics → Packet log** provides a bounded parsed packet list,
filters, search, detail and raw preview. The event terminal provides a bounded
structured log without credentials or remote command secrets. Basic board,
radio, storage, Wi-Fi, Map and crash state is available under Diagnostics.

Observer is optional and uses `mqtts://`, TLS, QoS 1 and a bounded queue. It
may publish device health and explicitly enabled location state. It never
publishes message text, keys, contacts or forwarded RF traffic.

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
routes trace status
wifi status
map tiles status
admin status
observer status
terminal status
crashlog
```

`help` lists the active allowlist. RF transmission, remote mutation, evidence
clearing, reboot and factory reset require explicit actions or confirmation.
The supported `ui tab <name>` product-navigation command is equivalent to a
local screen navigation: it wakes the display and clears the ordinary
tap-to-unlock idle cover before showing the requested page. It does not bypass
first-start setup, protected actions, or confirmation prompts.

DeskOS 1.5 also provides a read-only production framebuffer export for support
and documentation. It copies the current 480x480 RGB565 screen only; it cannot
transmit RF, format storage, or enable developer/qualification behavior. From a
repository checkout with `pyserial` and Pillow installed:

```sh
python scripts/ui_capture_d1l.py \
  --port /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
  --prep-command "ui tab home" \
  --png-out deskos-home.png \
  --out deskos-home.json
```

Valid tab names include `home`, `messages`, `nodes`, `map`, and `settings`.
Review the screen before capture: locations may be shown, but do not publish
private-message content, passwords, keys, or admin credentials.

## Installation

Use the guided browser flasher or the published DeskOS D1L 1.7.6 download and
follow its `START_HERE.md`.
On Linux, select the D1L only through the stable by-id path:

```text
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
VID:PID 1a86:7523
```

Never substitute `/dev/ttyUSB*` or a guessed Windows COM assignment. Normal
installation preserves NVS and never formats the SD card.
