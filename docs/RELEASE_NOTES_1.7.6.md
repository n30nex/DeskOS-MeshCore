# DeskOS MeshCore 1.7.6

DeskOS 1.7.6 completes first installation for the SenseCAP Indicator D1L and
adds a simple local-clock control.

## Highlights

- The NeonPocket browser flasher now opens the RP2040 BOOTSEL drive before any
  download, verifies its identity, checksum-verifies the matching bridge UF2,
  and copies it directly.
- The same onboarding page prepares an already-formatted FAT32 card with the
  required DeskOS directories and files.
- SD preparation accepts identical reruns, verifies every write, and refuses
  to replace a different existing file.
- Separate **Verify bridge** and **Verify in DeskOS** actions confirm the real
  D1L storage state instead of treating a file copy as success.
- **Settings -> Display** provides **Time -1h** and **Time +1h** controls for a
  persisted fixed local-clock offset.

## Safety

- Neither DeskOS nor the browser flasher formats an SD card.
- The bridge installer writes only after the selected folder identifies as an
  RP2040 BOOTSEL drive.
- The displayed timezone does not alter UTC radio, security, ordering, or
  retained timestamps. Daylight-saving changes remain manual.
- Firmware update installation preserves the existing DeskOS identity,
  contacts, settings, history, and unrelated retained flash regions.

## Validation

- Native settings and timezone-wrap tests.
- UI binding and touch-layout contracts.
- Browser mocks for RP2040 identity/write and non-destructive SD preparation.
- Complete host suite, exact GitHub Actions artifacts, and physical D1L checks
  are recorded with the published release.
