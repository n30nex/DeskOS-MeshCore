<p align="center">
  <img src="branding/deskos-mark-512.png" width="180" alt="DeskOS touch-display mesh mark">
</p>

<h1 align="center">DeskOS MeshCore</h1>

<p align="center"><strong>A bright, touch-first MeshCore desk for the SenseCAP Indicator D1L.</strong></p>

DeskOS **1.8.0-rc.3** is the release candidate for the SenseCAP Indicator D1L.
It uses the production `full_feature` profile with conditional SD-primary retained history
(`conditional` storage mode).

[Release candidate](https://github.com/n30nex/DeskOS-MeshCore/releases/tag/v1.8.0-rc.3)
· [Browser flasher](https://flasher.canadaverse.org/)
· [User guide](docs/USER_GUIDE_D1L.md)
· [Product page](https://canadaverse.org/deskos/)

The previous stable release is
[1.7.12](https://github.com/n30nex/DeskOS-MeshCore/releases/tag/v1.7.12).

## What the candidate improves

- WadaMesh is the interface parity target, with its exact reviewed revision and
  remaining differences in the [feature matrix](docs/DESKOS_MESHCORE_FEATURE_PARITY.md).
- **Chats** brings channels and recent direct messages onto the same page;
  the DMs list pages through every conversation in the retained message store.
- **Contacts** separates Saved and Discovered, adds role/favourite filters, and
  pages through every matching entry. Home and Contacts agree on heard totals.
- **Profile** exposes the node name, public identity, location and advert entry.
- Six editable **Quick replies** insert at the cursor for review before Send.
- Switching to Bluetooth closes Observer connections before releasing Wi-Fi,
  preventing a crash during an active MQTT connection attempt.
- Settings leads with device controls; keyboards use the dark palette. Display
  timeout offers 30 seconds through 10 minutes, and clock offsets use 15-minute steps.

- Signed SD updates verify the written flash image, honour cancellation before
  writing, and use the correct `deskos/updates` folder with clearer progress.
- The phone app's **View Telemetry** recognizes the D1L's local response.
- Slow LoRa profiles get a transmit deadline that covers their actual frame
  airtime; normal fast profiles retain the existing recovery deadline.
- Local SNR uses the correct units, and phone diagnostics show signal,
  direct/flood packet counts, receive errors, and calculated airtime.
- Wi-Fi signal updates without requiring a scan.
- A loading bar advances while saved history is restored.
- Map labels fit their space, avoid controls, preserve UTF-8 names, and open
  node details when tapped.
- The browser recognizes DeskOS JSON, waits for startup, verifies radio and
  identity readiness, and reads the correct SD status.
- An ordinary update can finish without optional SD storage. Fresh clean
  installations retain the complete three-stage verification.

See the [candidate release notes](docs/RELEASE_NOTES_1.8.0-rc.3.md) for the full
change and validation record. Earlier releases remain documented in the
[roadmap](docs/ROADMAP.md) and their release notes.

## Everyday use

The dark 480×480 touch interface has **Home**, **Chats**, **Contacts**,
**Map**, and **Settings**. It supports public and hashtag channels, direct
messages with acknowledgement state, contact search and sorting, discovery,
Ping and Trace, and authenticated repeater/room management.

Secure Bluetooth connects the official MeshCore phone app. Wi-Fi supports maps
and the opt-in Observer/MQTT uplink. Wi-Fi and Bluetooth are explicit exclusive
modes; cached maps and mesh messaging remain available in Bluetooth mode.

DeskOS is a non-forwarding client. Prepared FAT32 storage and the paired RP2040
bridge provide retained history and map caching. Missing storage is reported
as live-only operation, without silently moving history into default NVS.

## Install

Download the complete release ZIP, extract it, and open **START_HERE.md**, or use
the [guided browser flasher](https://flasher.canadaverse.org/).

- **Existing DeskOS:** choose the preserving update. The installer verifies the
  application before selecting its boot slot and preserves identity, contacts,
  settings, and SD data.
- **Fresh clean installation:** the full 8 MB image replaces ESP32 identity and
  settings. Complete the ESP32, RP2040 bridge, and prepared-card stages.
- **RP2040 bridge:** hold BOOTSEL while connecting its USB side and install the
  complete production UF2. The same UF2 serves update and fresh-install paths.
- **SD preparation:** use an already-formatted FAT32 card. Preparation adds only
  missing, verified files; it never formats the card or replaces different files.
- **Signed local update:** copy the matching manifest, signature, and app image
  under `deskos/updates/` on the prepared card, then use **Settings → Signed Update**.

The D1L can take tens of seconds to restore a populated card. Keep power and
USB connected while its loading/readiness screens finish.

Linux hardware operations use the stable D1L identity:

```text
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
VID:PID 1a86:7523
```

Do not substitute a guessed `/dev/ttyUSB*` path. See the
[guided installation notes](docs/D1L_SD_CARD_GUIDED_INSTALL.md).

## Interface and support

The [physical 1.7.5 reference gallery](docs/screenshots/device-1.7.5/README.md)
records the earlier on-device interface. Historical and simulated images are
labelled with their own versions; they are not current-candidate test evidence.

The [user guide](docs/USER_GUIDE_D1L.md) covers the current controls, USB support
commands, settings, and recovery. The [feature matrix](docs/DESKOS_MESHCORE_FEATURE_PARITY.md)
and [known limitations](docs/KNOWN_LIMITATIONS.md) explain the D1L adaptations.

## Security and recovery

Bluetooth requires authenticated, encrypted, bonded communication. It cannot
export/import private keys, factory-reset the D1L, or reboot it remotely.
Sharing is an explicit owner action. Signed SD updates verify the release and
require local confirmation before writing the inactive slot; an unhealthy
pending image rolls back. USB remains the recovery path.

The D1L has no onboard GPS or battery sensor. The UI is English-only. Remote
administration remains capability-gated, with local confirmation for changes.
See the [companion notes](docs/COMPANION_3BYTE_COMPATIBILITY.md).

## Build and release

Builds and checks run locally on the Pi 5 when requested by the maintainer.
Use the pinned ESP-IDF image, RP2040 core, exact source commit, production
profile, and existing update signer. The public package contains ESP32 update
and clean images, a complete RP2040 UF2, a signed update, checksums, source
provenance, an SBOM, and end-user instructions.

See [build provenance](docs/BUILD_PROVENANCE_D1L.md) and the
[release checklist](docs/RELEASE_CHECKLIST.md). Device backups, credentials,
private messages, and internal test material never belong in release assets.
