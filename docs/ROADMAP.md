# DeskOS release roadmap

## Release train

| Release | Product state | Repository state |
|---|---|---|
| **1.0 / RC1** | Published baseline (`v1.0.0`, corrected package `v1.0.1`) | Historical and complete |
| **1.2 / RC2** | Corrective release: working channel selection, mobile-style Contacts, complete parity ledger, actual-device screenshots, and explicit update/fresh-install downloads | Historical (`v1.2.0`) |
| **1.5 / RC3** | Secure BLE companion, signed local update/rollback, public-data QR sharing, diagnostics, and full-feature production activation | Current (`v1.5.0`) |

The release firmware is the ordinary public product. A controlled peer, Wi-Fi
credentials, admin password, soak run, qualification firmware, or validation
receipt is not an RC2 requirement.

## 1.0 / RC1 record

Issue #71 records the original release. The `v1.0.1` packaging correction added
clear update and full-clean paths without changing the product line.

| ID | Delivered result | State |
|---|---|---|
| R1 | Production `core_1_0` firmware and end-user package | Complete |
| R2 | ESP32 app BIN, full clean 8 MB BIN, complete RP2040 UF2, checksums, and `START_HERE.md` | Complete |
| R3 | Windows and Linux instructions for existing DeskOS and blank/non-DeskOS devices | Complete |
| R4 | Published and freshly downloaded public assets | Complete |

The public `v1.0.1` assets matched their staged files byte-for-byte. Known RC1
UI defects became the bounded RC2 work; they do not rewrite the historical RC1
record.

DeskOS supports the configured Map as normal product use; it is not a
release-lab prerequisite.

## 1.2 / RC2: corrective mobile parity

[Issue #322](https://github.com/n30nex/DeskOS-MeshCore/issues/322) controls RC2.
The current Android/iOS comparison and accepted D1L adaptations are recorded in
[`DESKOS_MESHCORE_FEATURE_PARITY.md`](DESKOS_MESHCORE_FEATURE_PARITY.md).

| ID | Work | Completion predicate | Current state |
|---|---|---|---|
| C0 | Mobile baseline audit | Every user-facing mobile area and primary action has a DeskOS outcome or an explicit D1L adaptation | Complete in parity ledger |
| C1 | Shared UI structure | Dark touch shell, list/detail hierarchy, selection, and primary actions are consistent and discoverable | Complete |
| C2 | Channels and chat | Selecting `#Public` or another enabled channel opens its conversation; read, send, search, unread, and channel-management flows remain usable | Complete for #320 |
| C3 | Contacts and nodes | Search, Recent/A-Z/Role/Signal sorting, node detail/status, repeater/room management, and companion DM actions are directly reachable | Complete for #321 |
| C4 | Remaining parity | Home, DMs, Finder, PATH/TRACE, Map, Wi-Fi, radio/device settings, administration, storage, diagnostics, and Observer have a documented normal-use outcome | Complete in parity ledger |
| C5 | Actual-device README images | Production read-only framebuffer export yields fresh 480x480 Home, Channels, Contacts, Map, and Settings PNGs from the attached D1L | Complete for #323; Map shows 9/9 local SD tiles |
| C6 | Public package | Exact Actions build contains update BIN, full clean 8 MB BIN, one complete RP2040 UF2, checksums, and current end-user instructions | Complete |
| C7 | Publication | Merge RC2, tag/release it, download every public asset again, and match published checksums | Complete (`v1.2.0`) |

RC2 is complete. Internal test plans remain developer material and are not
shipped in the production package or firmware.

## 1.5 / RC3: full-feature release

RC3 activates the product work deliberately held outside the corrective 1.2
profile. Its source is [`RC3_BACKLOG.md`](RC3_BACKLOG.md):

- BLE companion completion (#324);
- advanced QR, deep-link, and sharing workflows;
- signed OTA/update/rollback and on-device recovery;
- broad UI architecture work;
- optional telemetry and diagnostic expansion; and
- remaining feature and documentation debt already recorded in the backlog.

The production Actions candidate uses the `full_feature` profile. Pull requests
compile and test that profile without receiving the update-signing secret;
trusted branch builds alone produce the signed release package.
