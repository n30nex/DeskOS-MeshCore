# DeskOS release roadmap

## Release train

| Release | Product state | Repository state |
|---|---|---|
| **1.0 / RC1** | Published baseline (`v1.0.0`, corrected package `v1.0.1`) | Historical and complete |
| **1.2 / RC2** | Corrective release: working channel selection, mobile-style Contacts, complete parity ledger, actual-device screenshots, and explicit update/fresh-install downloads | Historical (`v1.2.0`) |
| **1.5 / RC3** | Secure BLE companion, signed local update/rollback, public-data QR sharing, diagnostics, and full-feature production activation | Historical (`v1.5.0`) |
| **1.6** | Compact Home, five Contacts sort modes, UI ownership cleanup, truthful simulator states, and touch/copy refinement | Historical (`v1.6.0`) |
| **1.7** | Branded animated opening, distinct DeskOS mark, and coherent product-wide palette | Historical (`v1.7.0`) |
| **1.7.1** | Direct repeater login, optional device-local passwords, command dashboard, and visible request results | Historical (`v1.7.1`) |
| **1.7.5** | Flood-delivered login, human-readable neighbours, manager return navigation, ten-minute lock, top-button wake/advert, and faster cached maps | Historical (`v1.7.5`) |
| **1.7.6** | Guided bridge/SD installation and adjustable local display time | Historical (`v1.7.6`) |
| **1.7.7** | Faster verified-advert admission and Contacts rendering | Historical (`v1.7.7`) |
| **1.7.8** | Dual MQTT uplink, Wi-Fi/BLE coexistence, live recency, map progress, and message time | Current (`v1.7.8`) |

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

## 1.6: daily-use refinement

DeskOS 1.6 keeps the 1.5 radio, BLE, storage, sharing, and signed-update
boundaries while improving the 480x480 product surface. Home now owns its
compact status bar and lock action, Contacts exposes search and five useful
sort orders, common actions use shorter labels, diagnostics fit without
clipping, and simulator state matches the full-feature firmware profile.

## 1.7: product identity

DeskOS 1.7 adds a non-blocking 3.2-second LVGL opening scene and applies the
DeskOS cyan, cobalt, lime, and charcoal palette across the complete interface.
The repository now carries the transparent production mark, deterministic boot
preview, and clearly labeled simulator images. The loader remains bounded and
falls through to Home if its small object tree cannot be allocated.

## 1.7.1: repeater management

DeskOS 1.7.1 puts **Login** beside managed repeater and room contacts, opens a
large masked password keyboard, and can remember a password per server on the
device. Authenticated sessions open a compact icon dashboard with focused
status, telemetry, neighbours, access, tools, room, and console pages. Every
request has a visible animated pending state and a persistent result. Saved
passwords are excluded from logs and exports and are removed when their contact
is forgotten or the device is factory-reset.

## 1.7.5: reliable remote management

DeskOS 1.7.5 sends every server login request by flood so a saved repeater can
be reached through another repeater without trusting a stale direct path.
Authenticated commands still use the normal learned route. Result pages return
to the signed-in manager, and Neighbours resolves saved names with readable age
and signal data. Cached map tiles render without network pacing. The display
locks after ten idle minutes, the lock screen owns the entire top layer, and the
top button wakes it or sends one normal advert on a deliberate double press.

## 1.7.7: faster radio-to-screen updates

DeskOS 1.7.7 publishes verified contact updates to the UI before ambient SD
persistence, then coalesces those writes through the existing retained-store
worker. User-initiated contact edits remain synchronous. Heard-node queries
also use an efficient library sort while retaining the same filters and order.

## 1.7.8: connectivity and screen-response repair

DeskOS 1.7.8 replaces the single status-only MQTT placeholder with the standard
two-broker MeshCore Canada packet uplink, using signed identity tokens and a
bounded handoff away from the radio/UI task. Wi-Fi and Bluetooth can coexist,
Recent Contacts reflects the current boot, map retries remain progressive, and
new channel messages retain a truthful local display time.
