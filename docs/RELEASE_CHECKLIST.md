# DeskOS D1L release checklist

This tracks public product deliverables. Controlled peers, credentials, admin
passwords, soak runs, internal evidence, and qualification firmware are not
release deliverables and must not appear in the public package.

The current production profile is `full_feature` with `conditional` storage:
SD-primary retained history when prepared storage is ready, visible live-only
operation otherwise, and without silent default-NVS fallback. Historical RC2
artifacts remain bound to their original `core_1_0` profile.

## 1.0 / RC1 record

- [x] `v1.0.0-rc.1`, `v1.0.0`, and the `v1.0.1` packaging correction are
  published.
- [x] The package provides an app update BIN, full clean 8 MB BIN, complete
  RP2040 UF2, checksums, and Windows/Linux instructions.
- [x] All nine `v1.0.1` public assets were freshly downloaded and matched
  staging byte-for-byte.

Release record: source `b796f5eeb080f520ab162e37430e69a1845dcfbe`, main
Actions run `31260655342`.

## 1.2 / RC2 corrective publication

### Product and documentation

- [x] The parity ledger covers every current mobile area and primary action.
- [x] #320 implementation opens the selected enabled channel conversation.
- [x] #321 implementation provides Contacts search, four useful sort orders,
  direct Message/Manage actions, companion DM, and repeater/room management.
- [x] The production framebuffer export is read-only and independent of test or
  qualification hooks.
- [x] Firmware and generated package docs identify version `1.2.0` from runtime
  source truth.
- [x] 1.0/RC1 remains historical; 1.2/RC2 is corrective; 1.5/RC3 remains the
  deferred expansion line.
- [x] README embeds fresh 480x480 Home, Channels, Contacts, Map, and Settings
  captures from the attached D1L (#323); Map was captured at 9/9 local SD tile
  cache hits and 9/9 rendered.

### Exact public artifacts

- [x] The exact final commit has a successful GitHub Actions build/package run.
- [x] The attached D1L runs production `1.2.0`; its version receipt identifies
  the exact Actions-built firmware commit. No separate validation firmware or
  controlled-peer gate is required.
- [x] The public package contains only production/user files and includes:
  - app update BIN for an existing DeskOS installation;
  - full clean 8 MB BIN for a blank/non-DeskOS device;
  - one complete RP2040 UF2 for either path;
  - `START_HERE.md`, user docs, and checksums.
- [x] PR #325 is merged and the exact main artifact is published as 1.2/RC2.
- [x] Every release asset is freshly downloaded and matches the published
  checksums.
- [x] Issues #320, #321, #322, and #323 are closed with links to the shipped
  release.

## 1.5 / RC3 boundary

BLE companion transport, richer QR/deep-link sharing, signed update/recovery,
localization expansion, broad UI architecture, optional telemetry, and debt in
[`RC3_BACKLOG.md`](RC3_BACKLOG.md) remain outside RC2.

## 1.6 release boundary

The 1.6 release keeps the established full-feature radio and storage safety
boundaries. Its release proof requires green host and Actions checks, exact-SHA
artifacts, a non-erasing flash to the stable D1L USB identity, and physical
480x480 UI acceptance. It never formats SD or substitutes an arbitrary serial
device.
