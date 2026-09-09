# DeskOS interface and MeshCore parity

This is the current product capability ledger for DeskOS 1.8.0-rc.5 on the
SenseCAP Indicator D1L. The original mobile baseline was reviewed on 2026-08-08 against the official
[MeshCore Android listing](https://play.google.com/store/apps/details?id=com.liamcottle.meshcore.android)
and [MeshCore iOS 1.47.0 listing](https://apps.apple.com/gb/app/meshcore/id6742354151).
It covers the user-facing capabilities and primary actions described by those
apps, including the recent channel search/settings, message search, contact
sorting/filtering/actions, repeater management, maps/path viewing, and sharing
work.

The official Android app **1.49.0** was also exercised with DeskOS **1.7.12**:
secure pairing/reconnect, contact synchronization, channel add/remove, incoming
DM, acknowledged outgoing DM, and repeater status/Unicode CLI replies passed.
Those are baseline interoperability results; candidate-specific results belong
in the corresponding tagged release record. No newer iOS device run is claimed.

The candidate corrects radio airtime deadlines, SNR units and companion
statistics, Wi-Fi signal refresh, early loading progress, map-label interaction,
and browser USB/storage verification. “Complete” below means implemented, not
that every phone, radio profile and physical recovery path has been tested.

## WadaMesh interface target — reviewed 2026-09-06

The maintainer selected [WadaMesh](https://github.com/ALLFATHER-BV/wadamesh/tree/8e94e250366632ee2e7a7198551e551aaa8b8024)
as the standalone interface parity target. This comparison uses exact source
`8e94e250366632ee2e7a7198551e551aaa8b8024`, its `doc-shots` screens, the
settings categories and chat/contact actions in `src/ui-touch/UITask.cpp`, and
the board/app documentation. DeskOS uses its own implementation and artwork;
no WadaMesh source, fonts or assets were copied into the firmware.

The target is a useful touch workflow on the 480×480 D1L. Existing phone
interoperability remains required. **This candidate does not claim complete
WadaMesh feature parity.** The older mobile-completion states below do not
close the additional differences in this table.

| WadaMesh area | DeskOS outcome | Current status |
|---|---|---|
| Chats landing with channels and DMs | Chats shows configured channels and recent DM conversations, unread counts, previews and delivery state; DMs pages through all conversations in the bounded retained store | Added in 1.8.0-rc.3; grouping is by conversation type |
| Conversation search, delivery, reply | Existing channel/DM history, search, explicit Send, ACK/retry status and message detail | Implemented, with editable plain-text quoted excerpts in 1.8.0-rc.4; mention picker remains future work |
| Quick-reply picker and editable macros | Six persistent replies, editable from Settings or the composer; insert at the cursor without replacing text or sending automatically; enforce the 138-byte UTF-8 message limit | Added in 1.8.0-rc.3 |
| Saved versus discovered contacts | Separate Saved/Discovered views; discovered excludes saved identities; accurate total/range and Previous/Next pages | Added in 1.8.0-rc.3; 12 rendered entries per page, up to the existing 64 saved / 512 heard limits |
| Role/favourite filter, sort, search | All, Chat, Repeaters, Rooms, Sensors and Favorites filters; Recent/Favorites/A-Z/Role/Signal sort; existing identity/name search | Added filters in 1.8.0-rc.3; discovery and filtering are navigation only |
| Contact detail, favourite, mute, share | Existing detail, alias, favourite/mute, remove confirmation, contact URI/QR and selected-contact messaging | Implemented |
| Auto-add policy controls | Existing verified-advert admission and bounded stores | Different policy; no user-selectable WadaMesh auto-add matrix yet |
| Profile | Name edit after onboarding, public identity, configured location and explicit advert entry from Settings | Added in 1.8.0-rc.3; no private-key import/export |
| Radio, Wi-Fi, Bluetooth, MQTT | Existing radio presets/custom controls, saved Wi-Fi profiles, encrypted/bonded phone companion, opt-in MQTT | Implemented; Wi-Fi and BLE are exclusive modes on DeskOS |
| Display and clock | Brightness, contrast/night, 30s/1m/2m/5m/10m/off display timeout and quarter-hour UTC offsets | Expanded in 1.8.0-rc.3; no automatic DST or global font-size picker |
| Settings navigation | Profile, Radio and Display & clock first; Connections, Storage & maps, Messaging, Tools and Support follow; About opens diagnostics | Updated in 1.8.0-rc.3; flat touch sections instead of WadaMesh's category grid |
| Map and offline cache | Existing pan/zoom/center, signed node locations, tile cache and attribution | Implemented; location is manual or authenticated companion data, not onboard GPS |
| Repeater/room management | Existing authenticated dashboard, status, telemetry, neighbours, ACL, room posts and console with explicit mutation confirmation | Implemented; radio reply/timeout limitations remain in the release record |
| Notifications and lock | Existing unread state, display pulse/quiet hours, touch lock and top-button wake | D1L adaptation; no audio playback or battery chart is claimed |
| Backups and updates | Preserving USB installer, explicit full-clean recovery image, signed local-SD inactive-slot update and rollback | D1L adaptation; physical signed-SD install/rollback still requires its own observation |
| Clipboard, inline quote, mention picker, per-thread drafts | On-device Copy/Paste, bounded plain-text Quote reply and exact-conversation SD drafts; clipboard clears on lock/restart | Added in 1.8.0-rc.4; no structured quote identifiers or mention picker |
| Language, emoji and keyboard options | English UI, bounded UTF-8 text and current bundled symbol coverage | Partial; no WadaMesh language/layout collection or full emoji artwork |
| GPS, battery, environmental sensors, audio | No onboard GPS or battery sensor; optional Indicator sensors are not integrated | Hardware/implementation differences; never substitute fabricated values |
| Lua app store/permissions, Reader, games, VNC and web remote UI | Existing USB console, browser installation and physical framebuffer export | Separate application/platform capabilities, not implemented WadaMesh parity |
| Spectrum/airtime applications | Existing packet log, signal and radio statistics, diagnostics and map | Partial; no continuous spectrum-scanner app |

Sources: [reference screens](https://github.com/ALLFATHER-BV/wadamesh/tree/8e94e250366632ee2e7a7198551e551aaa8b8024/doc-shots),
[UI actions and settings](https://github.com/ALLFATHER-BV/wadamesh/blob/8e94e250366632ee2e7a7198551e551aaa8b8024/src/ui-touch/UITask.cpp),
[supported boards](https://github.com/ALLFATHER-BV/wadamesh/blob/8e94e250366632ee2e7a7198551e551aaa8b8024/DEVICES.md).

## Official phone compatibility baseline

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
| Dark primary navigation | Persistent dark Home, Chats, Contacts, Map, and Settings dock with scrollable touch pages | Complete |
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
| Contact sort/filter | Cycle Recent, Favorites, A-Z, Role, and Signal ordering; role/favourite filters and search narrow the paged list | Complete (#321) |
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
| Observer integration | Opt-in dual secure MeshCore Canada packet/health uplink plus one custom broker; bounded off-radio queue, no RF forwarding or private-key export | Complete (1.7.9) |
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
