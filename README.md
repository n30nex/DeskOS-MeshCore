# MeshCore DeskOS for SenseCAP Indicator D1L

DeskOS **1.5 / RC3** is the current full-feature production firmware for the
SenseCAP Indicator D1L. Download `v1.5.0` from the
[GitHub release](https://github.com/n30nex/DeskOS-MeshCore/releases/tag/v1.5.0).
The Actions release is compiled with the immutable `full_feature` profile and
`conditional` SD history mode.

DeskOS is a standalone, dark, touch-first MeshCore client. Firmware is built
and packaged by GitHub Actions. The release provides the ESP32 update and
full-clean images, the complete RP2040 SD-bridge UF2, checksums, a signed local
update bundle, and end-user instructions.

## What 1.5 adds

DeskOS 1.5 includes the complete corrected 1.2 product plus:

- secure BLE companion pairing, bonding, reconnect, disconnect, and forget;
- the current MeshCore companion protocol over an encrypted BLE transport;
- deliberate one-time contact and channel QR sharing with public data only;
- Ed25519-signed local SD updates, inactive-slot installation, anti-downgrade
  sequencing, first-boot confirmation, and automatic rollback;
- expanded bounded diagnostics, event history, display preferences, and
  notification controls; and
- the corrected channel, Contacts, Finder, Ping, PATH, TRACE, Map, Wi-Fi,
  storage, administration, Observer/MQTT, and messaging workflows from 1.2.

DeskOS is a non-forwarding client. It sends user-requested traffic but does not
repeat other devices' traffic. Its conditional SD-primary retained history
becomes visibly live-only when storage is missing; history is never silently
redirected into default NVS.

## Security boundaries

- BLE requires Secure Connections, MITM protection, encryption,
  authentication, bonding, and an explicit notification subscription before
  the companion protocol becomes ready.
- BLE cannot export or import private keys, factory-reset the device, or reboot
  it remotely.
- QR codes contain only the public contact or channel material selected by the
  owner. Their temporary URI buffer is cleared after rendering.
- Signed updates are read from local SD, verified before the inactive slot is
  written, and require two deliberate on-device confirmations.
- USB remains the recovery path. DeskOS never formats the user's SD card.

See the [user guide](docs/USER_GUIDE_D1L.md),
[known limitations](docs/KNOWN_LIMITATIONS.md), and
[companion security/protocol notes](docs/COMPANION_3BYTE_COMPATIBILITY.md).

## Release train

| Release | Purpose | State |
|---|---|---|
| **1.0 / RC1** | Initial production baseline | Historical |
| **1.2 / RC2** | Channel, Contacts, parity, packaging, and screenshot correction | Historical (`v1.2.0`) |
| **1.5 / RC3** | BLE, signed update/rollback, sharing, diagnostics, and full-feature activation | Current (`v1.5.0`) |

## Device UI

These native 480x480 captures show the production touch shell introduced in
1.2 and retained by 1.5. Their exact 1.2 provenance is recorded in
[`docs/screenshots/DEVICE_1_2_CAPTURE.md`](docs/screenshots/DEVICE_1_2_CAPTURE.md).

| Home | Channels |
|---|---|
| ![DeskOS Home](docs/screenshots/device-1.2-home.png) | ![DeskOS Channels](docs/screenshots/device-1.2-channels.png) |

| Contacts | Settings |
|---|---|
| ![DeskOS Contacts](docs/screenshots/device-1.2-contacts.png) | ![DeskOS Settings](docs/screenshots/device-1.2-settings.png) |

![DeskOS Map](docs/screenshots/device-1.2-map-local-tiles.png)

Locations and public node labels may be visible. Private messages, passwords,
private keys, and admin credentials must never be included in screenshots.

## Install

Extract the complete release package and begin with `START_HERE.md`.

- **Update an existing DeskOS install:** use the app update path. It preserves
  unrelated retained flash regions.
- **Fresh clean install or recovery:** use the full 8 MB image at `0x0`.
- **RP2040 bridge:** copy the complete production UF2 through BOOTSEL.
- **On-device signed update:** place the exact manifest, signature, and app BIN
  from one release under `updates/` on the prepared SD card, then use
  **Settings -> Signed Update**.

On Linux, only the stable D1L identity is supported for flashing:

```text
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
VID:PID 1a86:7523
```

Never substitute a guessed `/dev/ttyUSB*` path. The firmware never formats an
SD card.
