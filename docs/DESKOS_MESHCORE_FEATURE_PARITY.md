# DeskOS MeshCore Mobile Parity

This is the **1.2 / RC2** parity ledger for the SenseCAP Indicator D1L.
DeskOS `v1.0.1` contains the source inventory below, but it has **not** achieved
form-and-function parity with the current MeshCore Android and iOS apps.
[Issue #322](https://github.com/n30nex/DeskOS-MeshCore/issues/322) controls the
audit and blocks RC2.

`Present; parity open` means code or a DeskOS workflow exists. It does not mean
the workflow is complete, discoverable, usable or equivalent to the mobile
apps. The audit must replace every open state with `Complete`, `Missing`, or an
explicitly accepted hardware-specific deviation.

## Known gaps

| Area | Current truth |
|---|---|
| Overall navigation and actions | Full Android/iOS comparison is not complete; tracked by #322. |
| Channels and chat | Selecting `#Public` can stop at a `channels queued` toast instead of opening chat; #320. |
| Contacts and nodes | Search, sorting and direct selected-node actions are incomplete; #321. |
| Actual-device documentation | Production serial framebuffer export and fresh README captures are missing; #323. |
| Remaining screens and workflows | Not accepted until inventoried under #322. |

## v1.0.1 source inventory (not parity acceptance)

### Client surface

| Capability | RC2 status | Current DeskOS workflow |
|---|---|---|
| Local identity and boot advert | Present; parity open | Factory-fresh units stay silent until onboarding saves an explicit device name; RX then starts before the first signed flood advert is queued. Later boots advertise only an already-onboarded retained identity. |
| Public and custom channels | Known defect #320 | Controls exist, but selecting `#Public` can queue channels without opening the chat. The complete mobile workflow remains open. |
| Direct messages | Present; parity open | Exact verified contact keys, direct/flood route selection, ACK correlation, bounded retry and terminal delivery state. |
| Contacts | Known gaps #321 | USB `contacts import <meshcore-uri>` plus on-device rename, favorite, mute and confirmed removal exist. Search, sorting and selected-node actions are incomplete. Heard-only or incomplete identities remain read-only; touchscreen URI import and QR sharing are not RC1 claims. |
| Heard nodes and roles | Present; parity open | Signed adverts populate at most 512 retained Chat, Repeater, Room, Sensor and Unknown rows without role inference. Capacity may replace only an unlocated least-recent entry; valid signed-location markers remain until the user explicitly clears the list. |
| Finder | Present; parity open | Zero-hop discovery lists unverified full keys and SNR evidence without promoting them to contacts until a signed advert arrives. |
| Ping, PATH and TRACE | Present; parity open | Repeater Ping is zero-hop TRACE; contact PATH/TRACE exposes pending, timeout, reply, RTT, RSSI and hop SNR state. |
| Map and node positions | Present; parity open | Centers on the configured device location and plots retained nodes with valid signed advert coordinates. Built-in OSM is attributed, visible-current-view-only, and limited to one 3×3 plan at one zoom per visible generation. |
| Automatic map download | Present; parity open | On connected Wi-Fi and qualified SD storage, an explicitly authorized provider may prefetch the bounds of nodes within 200 km. Zoom 8–18 is selected to fit the card budget, and prefetch pauses while interactive Map is open. |
| Multiple Wi-Fi profiles | Present; parity open | Scan, save, select, delete, connect, disconnect and reconnect from the device. |
| Radio and device settings | Present; parity open | Region/preset, frequency, bandwidth, spreading factor, coding rate, power, RX boost, display and time settings. |
| SD-first retained data | Present; parity open | SD is the primary history/map/export store. Missing or unusable SD enters a visible live-only RF chat mode; it does not silently move history into default NVS. |
| Packet and event diagnostics | Present; parity open | Bounded parsed packet/raw previews, event log, storage/map status and crash status without secret logging. |
| Observer MQTT | Present; parity open | Optional TLS-only, QoS 1, bounded health/location observer with no message text, keys or contacts. |

### Repeater and room administration

| Capability | RC2 status | Current DeskOS workflow |
|---|---|---|
| Verified target selection | Present; parity open | Repeater/room actions require an exact retained full key and canonical role. |
| Login and logout | Present; parity open | Masked password entry, empty-password repeater and room negotiation, peer-returned session permissions, explicit success/failure/timeout state and volatile authority cleared on logout or target switch. |
| Route selection | Present; parity open | Current proven direct route is preferred; bounded fallback and selected route state are visible. |
| Status and telemetry | Present; parity open | Authenticated status/counters and remote telemetry request/result are available on-device. |
| Neighbours | Present; parity open | Paged neighbour query with bounded full-key/contact resolution. |
| ACL | Present; parity open | Authenticated ACL query and confirmed mutations without displaying retained secrets. |
| Full CLI | Present; parity open | Bounded authenticated request/reply transcript with redaction and explicit local confirmation for mutations. Sensitive input is not persistently retained or logged, and volatile confirmation buffers are wiped. |
| Device/radio/advert settings | Present; parity open | Read/query through the admin surface; mutations require an authenticated matching session and local confirmation. |
| Room posts | Present; parity open | Current-session room text send/receive with bounded transcript and logout; old traffic is not replayed into a new session. |
| Remote destructive actions | Guarded; parity open | Reboot, password/key and other sensitive CLI mutations remain confirmation-gated; sensitive input is redacted, not persistently retained or logged, and wiped from volatile confirmation buffers. |

## Deferred expansion after RC2

These features are not shown as usable production controls and remain in the
1.5 / RC3 backlog. They do not expand RC2. The parity audit must record the
corresponding hardware-specific deviation or provide the equivalent normal-use
DeskOS outcome without pulling the deferred implementation forward:

- BLE companion pairing/transport (#324);
- on-device contact or channel QR sharing;
- signed OTA/update and recovery workflows.

URI-based channel/contact management, USB diagnostics and normal non-erasing
flashing remain available where described in the user guide.

## 1.2 / RC2 release gate

RC2 remains blocked by #322 until:

1. the exact current Android and iOS versions and every user-facing screen,
   capability and primary action are recorded here;
2. each row identifies the equivalent DeskOS location and state;
3. missing, confusing and dead-end workflows are corrected;
4. any genuine hardware-specific deviation is explicit and accepted;
5. #320, #321, #323 and every required child issue are closed;
6. fresh 480x480 screenshots from the exact production candidate on an actual
   D1L replace the temporary README notice; and
7. no required parity row remains open.

This is a product-completeness gate, not a controlled-peer, soak or test-firmware
campaign.
