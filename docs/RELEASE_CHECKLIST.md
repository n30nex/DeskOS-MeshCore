# DeskOS D1L 1.0 release checklist

This checklist is about the public product files. Internal developer checks,
controlled peers, admin credentials, soak runs, and evidence receipts are not
release deliverables.

The package is the production `core_1_0` profile. Its `conditional` storage
contract keeps SD primary and operates live-only without silent default-NVS
fallback when prepared bridge/card storage is unavailable.

- [ ] `manifest.json` identifies firmware version `1.0.0`, profile `core_1_0`,
  and the exact source commit.
- [ ] The package contains `firmware/bootloader.bin`,
  `firmware/partition-table.bin`, `firmware/ota_data_initial.bin`, and
  `firmware/meshcore_deskos_d1l.bin`.
- [ ] The package contains the one-file factory image under `full-flash/`.
- [ ] The package contains the production
  `rp2040/rp2040-sd-bridge-firmware/deskos_sd_bridge.ino.uf2`.
- [ ] `SHA256SUMS.txt` covers the complete package.
- [ ] `START_HERE.md` gives complete Windows and Linux instructions for SD
  preparation, RP2040 UF2 installation, ESP32 installation, first setup, and
  normal use.
- [ ] The package contains no internal test, smoke, evidence, debug, or audit
  files and the production firmware has qualification hooks disabled.
- [ ] The attached D1L is installed with the packaged normal installer and is
  used through its normal dark touch UI without erasing retained state.
- [ ] `v1.0.0-rc.1` publishes the ZIP, ESP32 BIN files, RP2040 UF2,
  `START_HERE.md`, and outer checksums.
- [ ] `v1.0.0` points to the same commit and publishes byte-identical copies of
  those assets.
- [ ] A fresh public download matches the published checksums and opens to a
  readable `START_HERE.md`.

The ZIP is the recommended end-user download. Standalone firmware files are
provided for experienced installers and recovery workflows.
