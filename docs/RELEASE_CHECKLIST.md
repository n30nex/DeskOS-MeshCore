# DeskOS D1L 1.0 release checklist

This checklist is about the public product files. Internal developer checks,
controlled peers, admin credentials, soak runs, and evidence receipts are not
release deliverables.

The package is the production `core_1_0` profile. Its `conditional` storage
contract keeps SD primary and operates live-only without silent default-NVS
fallback when prepared bridge/card storage is unavailable.

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
