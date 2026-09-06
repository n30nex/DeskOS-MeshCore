# DeskOS D1L 1.8.0-rc.2

This candidate keeps the 1.8.0 radio, Bluetooth and map improvements and
corrects problems found while exercising the signed-update path.

## Phone telemetry

The official app's four-byte request for local telemetry now receives the
documented self-telemetry response instead of an invalid-argument error and
misleading firmware-update prompt. The reply uses the device's own identity
and its existing 4.20 V wired-power compatibility value. It requires no RF
request; remote-node telemetry retains its authenticated request path.
The D1L still has no battery-voltage sensor or environmental telemetry in this
build.

## Signed SD installation

- The inactive flash image is read back and checked against its signed digest
  before it can become the next boot image. Replacing the SD file after its
  first verification cannot substitute another image.
- Queued cancellation is retained, and cancellation is checked atomically at
  the transition to writing. A ready update must be restarted before another
  install can begin.
- The three update files belong in `deskos/updates` on the card. Package
  destinations, signed-update ZIP layout and instructions identify that full
  path instead of incorrectly placing them at the card root.
- The update screen describes progress in plain language, identifies the SD
  folder and removes the install action during writing and final verification.

## Verification

Native tests exercise the production update state machine with changed SD
contents, failed flash readback, queued cancellation, cancellation at the write
boundary and repeated installation before reboot. The valid installation path
must still succeed. A signed-package check compares its destinations with the
actual bridge root and firmware paths.

The exact build, local test results and physical acceptance are recorded in the
[tagged release](https://github.com/n30nex/DeskOS-MeshCore/releases/tag/v1.8.0-rc.2).
Earlier phone acceptance on 1.8.0-rc.1 used official Android MeshCore 1.49.0:
secure reconnection, 36-contact synchronization, acknowledged two-way direct
messages, repeater login/status and a Unicode repeater-console reply. One
status request timed out; a fresh login and retry returned live metrics.

## Install

Use the preserving installer for an existing DeskOS device. The full 8 MB
image is an explicit clean installation that replaces ESP32 identity and
settings. SD files remain on the card. Signed updates use the existing
production signer. Builds and tests run locally on the Pi; no Actions build is
requested. Stable 1.7.12 remains available as the previous stable release.
