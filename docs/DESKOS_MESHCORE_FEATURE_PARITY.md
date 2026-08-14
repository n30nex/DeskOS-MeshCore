# DeskOS MeshCore mobile parity

This is the 1.5/RC3 product ledger for the SenseCAP Indicator D1L. The mobile
baseline was reviewed on 2026-08-08 against the official
[MeshCore Android listing](https://play.google.com/store/apps/details?id=com.liamcottle.meshcore.android)
and [MeshCore iOS 1.47.0 listing](https://apps.apple.com/gb/app/meshcore/id6742354151).
It covers the user-facing capabilities and primary actions described by those
apps, including the recent channel search/settings, message search, contact
sorting/filtering/actions, repeater management, maps/path viewing, and sharing
work.

State meanings:

- **Complete** — normal DeskOS product workflow is implemented.
- **Accepted D1L adaptation** — the standalone D1L provides a documented
  outcome instead of a phone/OS-specific workflow.
- **1.5 complete** - the full-feature production profile exposes the finished
  normal on-device workflow.

## Navigation and messaging

| Mobile capability / action | DeskOS location and outcome | RC2 state |
|---|---|---|
| App connection/onboarding | First-start on the D1L creates the local identity, optional location/Wi-Fi, radio preset, storage, and initial channels; no phone pairing is required | Accepted D1L adaptation |
| Dark primary navigation | Persistent dark Home, Channels, Contacts, Map, and Settings dock with scrollable touch pages | Complete |
| Home/status | Home summarizes identity, radio, storage, unread activity, connectivity, and shortcuts | Complete |
| Channel list and selection | Channels lists configured channels; tapping an enabled channel selects it and immediately opens its conversation | Complete (#320) |
| Public/channel conversation | Read retained history, send/receive messages, show sender and delivery state, and maintain unread state | Complete |
| Channel message search | Search the active retained conversation and return to normal history | Complete |
| Channel management | Create/import, select, enable/disable, rename, set default, and remove with confirmation | Complete |
| Channel settings | Channel actions are exposed from Channels rather than a mobile overflow menu | Accepted D1L adaptation |
| Direct-message list/thread | Verified chat contacts open a DM composer/thread with retained history, route, retry, ACK, and terminal delivery state | Complete |
| Direct-message search | Search retained DM history from the conversation surface | Complete |
| Composer behavior | Touch composer sends explicit user text; failed messages are not silently retried by navigation or refresh | Complete |
| Notifications/background app behavior | Unread state stays on-device; there is no host mobile OS notification/background process | Accepted D1L adaptation |

## Contacts, discovery, and administration

| Mobile capability / action | DeskOS location and outcome | RC2 state |
|---|---|---|
| Contacts list | Contacts shows the complete bounded saved-contact list with role, recency, and signal context | Complete |
| Contact search | Search name, role, fingerprint, or public key from Contacts | Complete (#321) |
| Contact sort/filter | Cycle Recent, A-Z, Role, and Signal ordering; search narrows the visible list | Complete (#321) |
| Selected contact actions | Obvious **Message** and **Login** actions open the DM or managed-server workflow | Complete (#321, #336) |
| Contact detail/edit | Inspect canonical identity and role; rename, favorite, mute, or remove a saved contact with confirmation | Complete |
| Companion DM | A verified Chat/Companion contact opens the existing DM compose/thread path | Complete (#321) |
| Repeater/room entry | A saved Repeater or Room exposes **Login** directly and from node detail even when no transient heard-node row is present | Complete (#321, #336) |
| Finder/discovery | Find sends zero-hop discovery and shows key, role, and there/back SNR without treating unverified results as contacts | Complete |
| PATH/Ping/TRACE | Verified contacts expose PATH/TRACE state; repeaters expose zero-hop Ping with pending, timeout, RTT, RSSI, and hop SNR results | Complete |
| Path/map relationship | Node/location detail and Map expose signed position truth; no position is inferred from a display name | Complete |
| Repeater/room login | Large masked keyboard, optional per-server device-local password, flood-delivered sign-in, empty-password negotiation, explicit session state/permissions, logout, and target-switch authority clearing | Complete (1.7.5) |
| Repeater status/telemetry | Authenticated icon dashboard, status, counters, telemetry, selected route, named paged neighbours, visible pending animation, persistent results, and return-to-manager navigation | Complete (1.7.5) |
| Repeater ACL/CLI/settings | Role-gated ACL and device/radio/advert actions plus bounded redacted CLI; mutations require local confirmation | Complete |
| Room posts | Current-session room posts and transcript; old room traffic is not replayed into a new session | Complete |

## Map, device, storage, and support

| Mobile capability / action | DeskOS location and outcome | RC2 state |
|---|---|---|
| Map and peer locations | Map pans/zooms/centers, plots only valid signed peer coordinates, and keeps provider attribution visible | Complete |
| Device location | Manual configured coordinates or supported signed data replace phone GPS; the D1L has no onboard GPS | Accepted D1L adaptation |
| Map download/cache | Connected Wi-Fi plus prepared SD and an authorized provider enable bounded background prefetch; interactive Map takes priority and cached tiles skip network pacing | Complete (1.7.5) |
| Wi-Fi profiles | Scan, save, select, delete, connect, disconnect, and reconnect from Settings | Complete |
| Radio/device settings | Region/preset, frequency, bandwidth, SF, CR, power, RX boost, display, and time settings | Complete |
| Storage/history | SD is primary for retained data; missing media produces visible live-only RF chat without silently moving history to default NVS | Complete |
| Packet/event diagnostics | Bounded packet detail/raw preview, event log, storage/Map/Wi-Fi/radio/crash state, and secret redaction | Complete |
| Observer integration | Opt-in TLS-only QoS 1 MQTT health/location observer; never publishes message text, keys, contacts, or forwarded traffic | Complete |
| Production screenshot/support export | Read-only 480x480 RGB565 framebuffer capture over the USB console; no RF transmit, storage format, test hook, or qualification mode | Complete (#323) |
| Accessibility/language | 480x480 touch layout, dark contrast, plain labels, and on-device keyboard; the current firmware is English-only | Accepted D1L adaptation; language expansion is RC3 |

## 1.5 full-feature conveniences

These mobile conveniences build on the corrected 1.2 workflows and retain the
same storage, identity, and local-authorization boundaries.

| Mobile convenience | RC2 outcome | State |
|---|---|---|
| BLE phone companion transport | Secure pairing, bonding, reconnect, disconnect, forget, and the bounded MeshCore companion protocol | 1.5 complete (#324) |
| QR/deep-link contact and channel sharing | Deliberate privacy-safe contact/channel QR export plus existing URI import and management | 1.5 complete |
| Mobile in-app firmware update | Signed local-SD inactive-slot install, anti-downgrade sequence, boot confirmation, rollback, and retained USB recovery | 1.5 complete |
| Phone OS localization and notifications | English on-device UI and on-device unread state | Accepted D1L adaptation |

## 1.5 completion

The software parity audit and corrected 1.2 product work remain complete. The
1.5 release adds the full-feature surfaces above and includes the Actions-built
update BIN, full clean BIN, complete RP2040 UF2, signed local update bundle,
instructions, and checksums.

No controlled peer, credentials, admin password, soak campaign, or special
release firmware is required.
