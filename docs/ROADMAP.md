# DeskOS release roadmap

## Shipped 1.0 line

Issue #71 records the original 1.0 release. The 1.0.1 packaging correction is
complete.

| ID | Work | Completion predicate | State / current blocker |
|---|---|---|---|
| D0 | Product scope | `core_1_0` is frozen; deferred features remain deferred | Complete |
| R1 | End-user package | Package and manifest say DeskOS 1.0 and contain no internal qualification material | Complete |
| R2 | Production artifacts | Exact main build yields ESP32 app/full BINs, production RP2040 UF2, checksums, and `START_HERE.md` | Complete |
| R3 | Normal installation | Install the exact package on the attached D1L using the same non-erasing user workflow documented in `START_HERE.md` | Complete |
| R4 | Ordinary use | Ship the runtime-documented dark touch UI for use on the owner's mesh; no controlled peer or lab receipt | Complete |
| R5 | RC1 publication | Publish `v1.0.0-rc.1` with the ZIP, BINs, UF2, checksums, and instructions | Complete |
| R6 | Stable publication | Publish the same artifact bytes as `v1.0.0`, verify public downloads, and close issue #71 | Complete |
| R7 | Explicit install paths | Name the app BIN update, full clean 8 MB BIN, and shared complete RP2040 UF2; provide Windows/Linux commands for both ESP32 paths | Complete |
| R8 | Packaging correction | Publish `v1.0.1` from the exact successful main package with explicit update/fresh asset names and instructions | Complete: commit `b796f5eeb080f520ab162e37430e69a1845dcfbe`, run `31260655342` |

The shipped product supports the configured Map as normal product use; it is not
a release-lab prerequisite.

The nine public `v1.0.1` assets were downloaded fresh and matched the staged
files byte-for-byte.

## 1.2 / RC2: corrective MeshCore mobile parity

[Issue #322](https://github.com/n30nex/DeskOS-MeshCore/issues/322) is the
controller and release blocker. RC2 must match the form and
function of the current MeshCore Android and iOS apps, adapted to the D1L
display without omitting core workflows. Execute the highest unblocked row.

| ID | Work | Completion predicate | State / current blocker |
|---|---|---|---|
| C0 | Mobile baseline audit | Inventory every Android/iOS screen, capability and primary action in `DESKOS_MESHCORE_FEATURE_PARITY.md`; mark each DeskOS row complete, missing or an accepted hardware-specific deviation | Ready: #322 open |
| C1 | Shared UI structure | Navigation, selection, list/detail hierarchy and action placement are predictable and touch-usable across the product | Blocked by C0 |
| C2 | Channels and chat | Resolve #320 and all C0 channel/chat gaps; selecting a channel opens it and normal read/send/search workflows are usable | Blocked by C0 |
| C3 | Contacts and nodes | Resolve #321 and all C0 contact/node gaps; search, sort, details/status, repeater login and companion DM actions are usable | Blocked by C0 |
| C4 | Remaining mobile parity | Resolve every required C0 gap across onboarding, home, map, radio, settings, administration, storage and diagnostics | Blocked by C0 |
| C5 | Actual-device README images | Resolve #323: add the minimal read-only production serial framebuffer export, then publish fresh 480x480 Home, Channels, Contacts, Map and Settings captures from the exact RC2 firmware on an actual D1L | Blocked by #323 |
| C6 | 1.2 / RC2 publication | Every required parity row and current defect is closed, actual-device screenshots are current, public install/update/full-reflash BIN and UF2 paths remain documented, and RC2 is published | Blocked by C1-C5 |

RC2 is ordinary public product work. It does not require a controlled peer,
credentials, soak, qualification firmware or a validation campaign.

## 1.5 / RC3: deferred expansion and debt

RC3 starts only after issue #322 and the 1.2/RC2 corrective gate are closed.
Its source of truth is [`RC3_BACKLOG.md`](RC3_BACKLOG.md) and the `1.5 / RC3`
GitHub milestone. It owns UI architecture (#6), developer-only quality tooling
(#17), telemetry expansion (#18), Finder/Ping/Trace polish (#19), advanced
QR/sharing (#20), signed update/recovery (#21), settings/schema work (#22),
documentation debt (#23), and BLE companion completion (#324). Those items
must not expand or delay the bounded RC2 correction.
