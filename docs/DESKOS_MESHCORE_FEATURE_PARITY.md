# DeskOS MeshCore Feature Parity

This is the production feature contract for the SenseCAP Indicator D1L
DeskOS 1.0 / RC1 firmware. `Ready` means the current source has an on-device
workflow and focused software coverage. It does not replace the final
exact-candidate hardware gate.

## RC1 client surface

| Capability | RC1 status | DeskOS workflow |
|---|---|---|
| Local identity and boot advert | Ready | Factory-fresh units stay silent until onboarding saves an explicit device name; RX then starts before the first signed flood advert is queued. Later boots advertise only an already-onboarded retained identity. |
| Public and custom channels | Ready | Create/import, select, enable, rename, make default, send/receive, search, unread state and confirmed removal. |
| Direct messages | Ready | Exact verified contact keys, direct/flood route selection, ACK correlation, bounded retry and terminal delivery state. |
| Contacts | Ready | USB `contacts import <meshcore-uri>` plus on-device rename, favorite, mute and confirmed removal. Heard-only or incomplete identities remain read-only; touchscreen URI import and QR sharing are not RC1 claims. |
| Heard nodes and roles | Ready | Signed adverts populate at most 512 retained Chat, Repeater, Room, Sensor and Unknown rows without role inference. Capacity may replace only an unlocated least-recent entry; valid signed-location markers remain until the user explicitly clears the list. |
| Finder | Ready | Zero-hop discovery lists unverified full keys and SNR evidence without promoting them to contacts until a signed advert arrives. |
| Ping, PATH and TRACE | Ready | Repeater Ping is zero-hop TRACE; contact PATH/TRACE exposes pending, timeout, reply, RTT, RSSI and hop SNR state. |
| Map and node positions | Ready | Centers on the configured device location and plots retained nodes with valid signed advert coordinates. Built-in OSM is attributed, visible-current-view-only, and limited to one 3×3 plan at one zoom per visible generation. |
| Automatic map download | Ready | On connected Wi-Fi and qualified SD storage, an explicitly authorized provider may prefetch the bounds of nodes within 200 km. Zoom 8–18 is selected to fit the card budget, and prefetch pauses while interactive Map is open. |
| Multiple Wi-Fi profiles | Ready | Scan, save, select, delete, connect, disconnect and reconnect from the device. |
| Radio and device settings | Ready | Region/preset, frequency, bandwidth, spreading factor, coding rate, power, RX boost, display and time settings. |
| SD-first retained data | Ready | SD is the primary history/map/export store. Missing or unusable SD enters a visible live-only RF chat mode; it does not silently move history into default NVS. |
| Packet and event diagnostics | Ready | Bounded parsed packet/raw previews, event log, storage/map status and crash status without secret logging. |
| Observer MQTT | Ready | Optional TLS-only, QoS 1, bounded health/location observer with no message text, keys or contacts. |

## Repeater and room administration

| Capability | RC1 status | DeskOS workflow |
|---|---|---|
| Verified target selection | Ready | Repeater/room actions require an exact retained full key and canonical role. |
| Login and logout | Ready | Masked password entry, empty-password repeater and room negotiation, peer-returned session permissions, explicit success/failure/timeout state and volatile authority cleared on logout or target switch. |
| Route selection | Ready | Current proven direct route is preferred; bounded fallback and selected route state are visible. |
| Status and telemetry | Ready | Authenticated status/counters and remote telemetry request/result are available on-device. |
| Neighbours | Ready | Paged neighbour query with bounded full-key/contact resolution. |
| ACL | Ready | Authenticated ACL query and confirmed mutations without displaying retained secrets. |
| Full CLI | Ready | Bounded authenticated request/reply transcript with redaction and explicit local confirmation for mutations. Sensitive input is not persistently retained or logged, and volatile confirmation buffers are wiped. |
| Device/radio/advert settings | Ready | Read/query through the admin surface; mutations require an authenticated matching session and local confirmation. |
| Room posts | Ready | Current-session room text send/receive with bounded transcript and logout; old traffic is not replayed into a new session. |
| Remote destructive actions | Guarded | Reboot, password/key and other sensitive CLI mutations remain confirmation-gated; sensitive input is redacted, not persistently retained or logged, and wiped from volatile confirmation buffers. |

## Intentionally deferred to 1.5 / RC2

These features are compiled behind unavailable RC1 capabilities and are not
shown as usable production controls:

- BLE companion pairing/transport;
- on-device contact or channel QR sharing;
- signed OTA/update and recovery workflows.

URI-based channel/contact management, USB diagnostics and normal non-erasing
flashing remain available where described in the user guide.

## Final release gate

Software scope is complete only when the exact RC1 commit builds in GitHub
Actions with `D1L_RELEASE_PROFILE=core_1_0` and
`D1L_SD_HISTORY_MODE=conditional`. The downloaded checksum, inventory and
provenance must verify before flashing. Public release still requires one
bounded physical gate on that exact Actions artifact:

1. boot and DeskOS navigation;
2. boot identity advert and one Public message;
3. one DM with ACK;
4. one PATH/TRACE and one repeater Ping;
5. repeater login plus authenticated query;
6. Wi-Fi reconnect;
7. SD write/remount and degraded-mode notice check;
8. one authorized Map download followed by an offline cache revisit.

No soak is part of this release run. The only permitted D1L route is
`/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0` on the Pi 5 after verifying
`VID:PID 1a86:7523`.
