# DeskOS MeshCore Feature Parity

This is the production feature gate for the SenseCAP Indicator D1L DeskOS
firmware. A feature is not complete merely because a protocol handler exists:
it must be reachable on the device, show truthful state, survive its expected
lifecycle, and pass focused Pi 5 plus exact-candidate hardware acceptance.

## Comparison baseline

The audit on 2026-07-25 compares DeskOS with:

- the official MeshCore companion protocol and current MeshCore firmware;
- MeshCore Open, including its repeater/room management implementation
  (`0fe250230905fdd05dbedc0f546736990beacf53`);
- MeshCoreTerm / MC Term v0.9.13 documentation and changelog
  (`bfb7a3b6d2aa30907f7b38992502e62510eab2cf`);
- the official iOS/Android companion-client management surface.

`Ready` means the current DeskOS source has an on-device path and focused
software coverage. It does not replace exact release-candidate hardware
evidence. `Blocking` means production release is forbidden.

## Client capability matrix

| Capability | Phone client | MeshCoreTerm | DeskOS status | Production requirement |
|---|---:|---:|---|---|
| Local identity, name, QR/export and boot advert | Yes | Yes | Ready | Exact candidate must boot as `D1L` and advertise that identity |
| Public and custom channel messaging | Yes | Yes | Ready | Send, receive, history, search and unread state |
| Direct messages with ACK/delivery state | Yes | Yes | Ready | Direct/flood route selection, retry and terminal delivery state |
| Contacts: import/export, rename, favorite, mute, delete | Yes | Yes | Ready | Only verified full keys become send targets |
| Heard nodes, roles, signal and route detail | Yes | Yes | Ready | Unknown/noncanonical identities stay read-only |
| Path discovery/reset, ping and TRACE | Yes | Yes | Partial | TRACE exists; expose equivalent route reset and reachability actions |
| Map, peer positions and explicit local position | Yes | Yes | Ready | No invented GPS; signed peer location only |
| Telemetry request/history | Yes | Yes | Partial | Local telemetry exists; remote manual request/history must be on-device |
| Multiple Wi-Fi profiles | Platform dependent | Yes | Partial | Current saved station works; profile selection remains to be exposed |
| BLE/Wi-Fi companion operation | Yes | Yes | Ready | Official core companion surface and authenticated BLE |
| Radio, display, notification and storage settings | Yes | Yes | Ready | Persisted user-visible settings and truthful hardware state |
| Packet/raw diagnostic inspection | Varies | Yes | Ready | Bounded parsed packet and event views; no credential logging |
| Signed firmware update and rollback | Platform dependent | No online update | Ready | Exact signed local package only |

## Repeater and room management matrix

| Capability | Phone client | MeshCoreTerm | DeskOS status | Production requirement |
|---|---:|---:|---|---|
| Select verified repeater/room target | Yes | Yes | Ready | Exact retained full key and role |
| Password login from device | Yes | Yes | In progress | Masked input, empty repeater password, clear failure/success state |
| Saved-route direct login with flood fallback | Yes | Yes | Ready in runtime | Show selected route mode and timeout state in UI |
| Guest/admin role gating | Yes | Yes | Partial | Read tools for guest; all mutations admin-gated |
| Logout and target-switch session clearing | Yes | Yes | In progress | Clear volatile authority when leaving or switching |
| Detailed status and counters | Yes | Yes | Partial | Render all decoded repeater/room status fields |
| Telemetry request | Yes | Yes | Blocking | Request, decode and display remote telemetry |
| Neighbour list | Yes | Yes | Blocking | Paged binary neighbour request and contact resolution |
| Full CLI console | Yes | Yes | Blocking | Bounded command/reply transcript with sensitive-command redaction |
| Device/radio/advert settings | Yes | Yes | Blocking | Read/write through authenticated CLI with local confirmation |
| Region management | Yes | Yes | Blocking | List/add/remove regions with role checks and confirmation |
| ACL and guest/admin management | Yes | Yes | Blocking | List/add/update/remove ACL entries without exposing secrets |
| Server identity management | Yes | Yes | Blocking | Name/location/public identity plus guarded private-key operations |
| Password management | Yes | Yes | Blocking | Masked entry, no retained/logged/echoed secret |
| Reboot | Yes | Yes | Blocking | Explicit second local confirmation |
| Room login | Yes | Yes | In progress | Current session login must not replay old traffic unexpectedly |
| Room console transcript/send | Yes | Yes | Blocking | New posts, ACK state, bounded current-session transcript and logout |

## Release gate

Production remains blocked until every `Blocking`, `Partial`, and `In progress`
row above is either:

1. implemented with an on-device path and focused automated coverage; or
2. documented as genuinely hardware-inapplicable while providing the
   equivalent DeskOS workflow.

The final candidate must then be built by GitHub Actions, flashed on the Pi 5
through the stable D1L USB identity, and pass the required smoke, persistence,
SD, Wi-Fi, controlled-peer RF/admin, automated scroll, and framebuffer gates.
No soak is part of this release run.
