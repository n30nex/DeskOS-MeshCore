# DeskOS D1L 1.0 release checklist

This checklist is about the public product files. Internal developer checks,
controlled peers, admin credentials, soak runs, and evidence receipts are not
release deliverables.

The package is the production `core_1_0` profile. Its `conditional` storage
contract keeps SD primary and operates live-only without silent default-NVS
fallback when prepared bridge/card storage is unavailable.

- [ ] `manifest.json` identifies firmware version `1.0.1`, profile `core_1_0`,
  and the exact source commit.
- [ ] The package contains `firmware/bootloader.bin`,
  `firmware/partition-table.bin`, `firmware/ota_data_initial.bin`, and
  `firmware/meshcore_deskos_d1l.bin`.
- [ ] The app BIN is explicitly documented as the update for existing DeskOS;
  the package also contains the full clean image under `full-flash/` for
  blank/non-DeskOS devices.
- [ ] The package contains the production
  `rp2040/rp2040-sd-bridge-firmware/deskos_sd_bridge.ino.uf2`.
- [ ] `SHA256SUMS.txt` covers the complete package.
- [ ] `START_HERE.md` gives complete Windows and Linux instructions for the app
  BIN update, full clean BIN, one shared complete RP2040 UF2, SD preparation,
  first setup, and normal use.
- [ ] The package contains no internal test, smoke, evidence, debug, or audit
  files and the production firmware has qualification hooks disabled.
- [x] `v1.0.0-rc.1` and `v1.0.0` remain published as the original 1.0 release.
- [ ] `v1.0.1` publishes the ZIP, explicitly named update and full-clean BINs,
  the shared RP2040 UF2, `START_HERE.md`, and outer checksums.
- [ ] A fresh public download matches the published checksums and opens to a
  readable `START_HERE.md`.

The ZIP is the recommended end-user download. Standalone firmware files are
provided for experienced installers and recovery workflows.
