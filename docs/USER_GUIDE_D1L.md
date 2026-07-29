# MeshCore DeskOS D1L 1.0 User Guide

This guide covers the production `core_1_0` firmware with `conditional` SD
history for the Seeed SenseCAP Indicator D1L. DeskOS is a non-forwarding
MeshCore client: it sends and receives user-requested traffic but does not
repeat other devices' traffic.

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
NRCan provider manifest are required for the complete 1.0 setup.

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

The Public channel is always present. DeskOS also supports custom channels:
create or import, select, enable, rename, make default and remove with local
confirmation. QR sharing is intentionally absent from 1.0; URI import remains
available.

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
`contacts import <meshcore-uri>`. The touchscreen supports rename, favorite,
mute and confirmed removal. Contact and channel QR sharing is not part of 1.0.

From **Contacts**:

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

## Repeater and room administration

Open a verified Repeater or Room node and choose **Admin**. Login uses the
exact retained server key and a masked password. Repeater and room logins may
request empty-password negotiation; the peer decides whether to accept it and
returns the session permissions (guest, read-only, write or admin). Leaving,
logging out or switching targets clears volatile session authority.

The on-device admin surface provides:

- login, logout, session state and selected route;
- detailed status/counters and telemetry;
- paged neighbours;
- ACL query and confirmed ACL mutations;
- bounded authenticated CLI request/reply;
- device, radio and advert queries/mutations with local confirmation;
- room posts and current-session transcript.

Guest/admin role checks are enforced locally and by the remote response.
Sensitive commands and passwords are not persistently retained or logged;
sensitive replies are redacted and volatile confirmation input is wiped.
Remote mutations require a second local confirmation.
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
- Prepare a 32GB-or-larger FAT32 card with the checked-in
  `scripts/prepare_deskos_sd.py` workflow. The 1.0 first-start flow requires
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

## Not included in 1.0

DeskOS D1L 1.0 hides and rejects:

- BLE companion pairing and transport;
- contact/channel QR sharing;
- signed OTA/update and recovery workflows.

Normal non-erasing flashing and bounded USB recovery diagnostics are separate
from those deferred product workflows.

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

## Installation

Use the published DeskOS D1L 1.0 download and follow its `START_HERE.md`.
On Linux, select the D1L only through the stable by-id path:

```text
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
VID:PID 1a86:7523
```

Never substitute `/dev/ttyUSB*` or a guessed Windows COM assignment. Normal
installation preserves NVS and never formats the SD card.
