# DeskOS D1L release checklist

This checklist is about the public product files. Internal developer checks,
controlled peers, admin credentials, soak runs, and evidence receipts are not
release deliverables.

## Completed 1.0 / RC1 record

The shipped package is the production `core_1_0` profile. Its `conditional`
storage contract keeps SD primary and operates live-only without silent
default-NVS fallback when prepared bridge/card storage is unavailable.

- [x] `manifest.json` identifies firmware version `1.0.1`, profile `core_1_0`,
  and the exact source commit.
- [x] The package contains `firmware/bootloader.bin`,
  `firmware/partition-table.bin`, `firmware/ota_data_initial.bin`, and
  `firmware/meshcore_deskos_d1l.bin`.
- [x] The app BIN is explicitly documented as the update for existing DeskOS;
  the package also contains the full clean image under `full-flash/` for
  blank/non-DeskOS devices.
- [x] The package contains the production
  `rp2040/rp2040-sd-bridge-firmware/deskos_sd_bridge.ino.uf2`.
- [x] `SHA256SUMS.txt` covers the complete package.
- [x] `START_HERE.md` gives complete Windows and Linux instructions for the app
  BIN update, full clean BIN, one shared complete RP2040 UF2, SD preparation,
  first setup, and normal use.
- [x] The package contains no internal test, smoke, evidence, debug, or audit
  files and the production firmware has qualification hooks disabled.
- [x] `v1.0.0-rc.1` and `v1.0.0` remain published as the original 1.0 release.
- [x] `v1.0.1` publishes the ZIP, explicitly named update and full-clean BINs,
  the shared RP2040 UF2, `START_HERE.md`, and outer checksums.
- [x] A fresh public download matches the published checksums and opens to a
  readable `START_HERE.md`.

The ZIP is the recommended end-user download. Standalone firmware files are
provided for experienced installers and recovery workflows.

Release record: `v1.0.1` was built from
`b796f5eeb080f520ab162e37430e69a1845dcfbe` by successful main run
`31260655342`; all nine public assets matched staging byte-for-byte.

## 1.2 / RC2 corrective publication gate

RC2 is blocked by [issue #322](https://github.com/n30nex/DeskOS-MeshCore/issues/322).
These are public product deliverables, not a controlled-peer, soak or
qualification campaign.

- [ ] The parity ledger covers every current Android/iOS screen, primary action
  and equivalent DeskOS location.
- [ ] #320 is closed: selecting `#Public` opens the usable Public chat.
- [ ] #321 is closed: Contacts provides usable search, sorting, node details,
  repeater status/login and companion direct-message actions.
- [ ] Every other current product defect and required parity gap recorded under
  #322 is closed or has an explicitly accepted D1L-specific deviation.
- [ ] #323 is closed: the main README embeds high-quality 480x480 Home,
  Channels, Contacts, Map and Settings screenshots captured over serial from
  an actual D1L running the exact RC2 production candidate.
- [ ] The production screenshot path is read-only and does not re-enable test
  or qualification firmware.
- [ ] The public package still provides an app update BIN, a full clean 8 MB
  reflash BIN, the complete RP2040 UF2, checksums and end-user instructions for
  blank devices and existing DeskOS installations.
- [ ] README, roadmap, scope, limitations, user guide, milestones, issues and
  release notes all identify 1.0/RC1 as shipped and 1.2/RC2 as corrective.
- [ ] No 1.5/RC3 feature or technical-debt campaign has expanded RC2.

## 1.5 / RC3 boundary

BLE companion transport, signed update/recovery, advanced sharing, broad UI
architecture, telemetry expansion, optional polish and longer-term debt remain
in [`RC3_BACKLOG.md`](RC3_BACKLOG.md) until RC2 is complete.
