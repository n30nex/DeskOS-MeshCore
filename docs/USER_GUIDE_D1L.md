# MeshCore DeskOS D1L 1.0 / RC1 User Guide

This guide covers the production `core_1_0` firmware for the Seeed SenseCAP
Indicator D1L. DeskOS is a non-forwarding MeshCore client: it sends and
receives user-requested traffic but does not repeat other devices' traffic.

## First start

1. Power on the D1L and complete onboarding.
2. Set the device name and confirm the authorized radio profile.
3. Open **Tools → Identity** and confirm the retained identity is ready.
4. Insert a prepared FAT32 DeskOS SD card.
5. Open **Tools → Storage** and confirm the card and data root are ready.
6. Configure Wi-Fi if automatic Map downloads are wanted.

The dock is **Home**, **Messages**, **Nodes**, **Map**, and **Tools**. Home is a
summary page; lists and long sheets scroll vertically.

After MeshCore receive starts, DeskOS queues a signed flood advert for the
retained device identity. This lets nearby clients attribute later Public
messages instead of showing only `Unknown`.

## Messages, channels and direct messages

The Public channel is always present. DeskOS also supports custom channels:
create or import, select, enable, rename, make default and remove with local
confirmation. QR sharing is intentionally absent from RC1; URI import remains
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
mute and confirmed removal. Contact and channel QR sharing is deferred to RC2.

From **Nodes**:

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

Open **Tools → Connections → Wi-Fi** to scan, save profiles, select one,
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
  `scripts/prepare_deskos_sd.py` workflow. A provider manifest is optional and
  is required only for authorized background/offline Map download.
- Foreign, non-FAT32 or unmountable media is preserved and reported.
- A missing/unusable card activates a prominent degraded notice.
- Degraded mode keeps basic live RF Public/channel/DM chat available in a
  live-only session, but retained history, Map download/cache and exports are
  unavailable.
- DeskOS does not silently redirect history into default NVS.

Use **Tools → Storage** before removing media or diagnosing a card.

## Diagnostics and Observer

**Tools → Packets** provides a bounded parsed packet list, filters, search,
detail and raw preview. The event terminal provides a bounded structured log
without credentials or remote command secrets. Basic board, radio, storage,
Wi-Fi, Map and crash state is available under Diagnostics.

Observer is optional and uses `mqtts://`, TLS, QoS 1 and a bounded queue. It
may publish device health and explicitly enabled location state. It never
publishes message text, keys, contacts or forwarded RF traffic.

## Deferred to 1.5 / RC2

The RC1 profile hides and rejects:

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

## Install and release evidence

Production release firmware must come from the exact GitHub Actions run for
the candidate commit, using `D1L_RELEASE_PROFILE=core_1_0` and
`D1L_SD_HISTORY_MODE=conditional`. Verify its checksum tree, inventory and
provenance before flashing. The current hardware is connected to Pi 5 host
`neopi5` and must be selected only through:

```text
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
VID:PID 1a86:7523
```

Never substitute `/dev/ttyUSB*` or a stale Windows COM assignment. Normal
flashing must not erase NVS or format/touch SD.

Feature implementation and public-release authorization are separate. The
release is ready only after the exact downloaded Actions artifact is flashed
and that same commit/artifact passes the bounded
boot/UI, advert/Public, DM/ACK, PATH/TRACE/Ping, admin, Wi-Fi reconnect, SD
write/remount/degraded and Map download/cache-revisit checks. No soak is
required for this release.
