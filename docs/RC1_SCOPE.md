# DeskOS 1.0 RC1 scope

RC1 is the compiled and packaged `core_1_0` release profile. Source feature
flags and the generated package manifest remain the compiled truth; this file
is their concise human contract.

| State | Surface | RC1 contract |
|---|---|---|
| Included and release-gated | D1L board, display, touch, backlight, Home, and core navigation | The exact Actions firmware and package must expose the bounded touch-first product surface. |
| Included and release-gated | Public/channel messaging, DM, contacts, Nodes, packets, ACK, PATH, Ping, route, and signal | Final sources prove the required controlled RF and protocol behavior on the exact candidate. |
| Included and release-gated | Radio settings, identity, multichannel management, controlled administration, user trace, and mutable terminal actions | Unsupported or unauthorized operations fail closed before side effects. |
| Included and release-gated | User Wi-Fi, configured location, Map, provider attribution, current diagnostics, and opt-in observer/MQTT | The final Map source proves an authorized fresh download and SD cache revisit. |
| Included but degraded/conditional | SD-primary retained history and Map cache | With the paired bridge, prepared FAT32 card, and authorized provider, SD is primary. Without required media, the UI reports live-only operation and does not silently redirect retained history into default NVS. The firmware never formats the card. |
| Included but degraded/conditional | Location and time | The D1L has no onboard GPS. Location is configured or obtained from supported signed data; age and time claims remain unavailable until a trusted source exists. |
| Included but degraded/conditional | Installation and recovery | Normal host-side USB installation is supported. The package includes a checksum-verified host recovery image; recovery is explicit and may overwrite retained state. |
| Deferred from RC1 | BLE companion transport | Continue only as reviewed RC2 work. |
| Deferred from RC1 | Signed OTA, update, rollback, and on-device recovery | USB remains the RC1 installation and recovery path. |
| Deferred from RC1 | Advanced QR/contact/channel sharing and emoji work | No deferred sharing polish is an RC1 gate. |
| Deferred from RC1 | Broad UI architecture refactoring | RC1 changes stay bounded to observed release defects. |
| Deferred from RC1 | Telemetry expansion beyond current release diagnostics | Existing release diagnostics are the RC1 boundary. |
| Deferred from RC1 | Feature additions discovered during qualification | Record them in `RC2_BACKLOG.md` unless they are a reproducible RC1 defect. |

Changing this contract requires explicit maintainer approval and matching
changes to the compiled release profile and package contract.
