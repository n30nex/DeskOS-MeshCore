# DeskOS 1.0 / RC1 product scope

DeskOS 1.0 / RC1 is the shipped compiled and packaged `core_1_0` release
profile. Source feature flags and the generated package manifest remain the
runtime truth; this file is their concise human contract.

| State | Surface | 1.0 contract |
|---|---|---|
| Included | D1L board, display, touch, backlight, Home, and core navigation | The dark 480x480 touch UI uses Home, Channels, Contacts, Map, and Settings as its primary navigation. |
| Included | Public/channel messaging, DM, contacts, Nodes, packets, ACK, PATH, Ping, route, and signal | These work on the user's own compatible MeshCore network; no project-operated peer is required. |
| Included | Radio settings, identity, multichannel management, administration, user trace, and terminal actions | Remote actions still require the permissions and confirmations shown by the product. |
| Included | User Wi-Fi, configured location, Map, provider attribution, diagnostics, and opt-in observer/MQTT | Mesh messaging does not require Wi-Fi. Online Map download uses the user's configured Wi-Fi and provider; cached maps remain available offline. |
| Included but degraded/conditional | SD-primary retained history and Map cache | With the paired bridge, prepared FAT32 card, and authorized provider, SD is primary. Without required media, the UI reports live-only operation and does not silently redirect retained history into default NVS. The firmware never formats the card. |
| Included but degraded/conditional | Location and time | The D1L has no onboard GPS. Location is configured or obtained from supported signed data; age and time claims remain unavailable until a trusted source exists. |
| Included but degraded/conditional | Installation and recovery | Normal host-side USB installation is supported. The package includes a checksum-verified host recovery image; recovery is explicit and may overwrite retained state. |
| Deferred from 1.0 | BLE companion transport | Continue only as later reviewed work. |
| Deferred from 1.0 | Signed OTA, update, rollback, and on-device recovery | USB remains the 1.0 installation and recovery path. |
| Deferred from 1.0 | Advanced QR/contact/channel sharing and emoji work | These are not part of the 1.0 product. |
| Deferred from 1.0 | Broad UI architecture refactoring | The current working UI is the 1.0 product surface. |
| Deferred from 1.0 | Telemetry expansion beyond current diagnostics | Existing diagnostics are the 1.0 boundary. |
| Corrective after 1.0 | Reproducible 1.0 defects and required mobile-parity gaps | Fix in 1.2 / RC2 under issue #322; do not defer them as later features. |
| Deferred from 1.0 | New features, architecture work and technical debt | Record them in `RC3_BACKLOG.md` for 1.5 / RC3 unless they are required to close a 1.2 / RC2 parity gap. |

Changing this contract requires explicit maintainer approval and matching
changes to the compiled release profile and package contract.

Known post-release defects in this baseline are recorded in
[`KNOWN_LIMITATIONS.md`](KNOWN_LIMITATIONS.md). Their presence does not rewrite
the historical 1.0/RC1 artifact record; they block 1.2/RC2 until corrected.
