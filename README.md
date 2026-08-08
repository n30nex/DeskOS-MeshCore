# MeshCore DeskOS for SenseCAP Indicator D1L

DeskOS 1.0 is the production `core_1_0` firmware for the SenseCAP Indicator
D1L. GitHub Releases provides the complete user package, standalone ESP32 BIN
files, the RP2040 UF2, checksums, and installation instructions.

DeskOS is the on-device MeshCore interface for the SenseCAP Indicator D1L. The
public 1.0 binaries use the bounded `core_1_0` profile built and packaged by
GitHub Actions.

## 1.0 feature surface

- touch-first Home and core navigation;
- Public and channel messaging, direct messages, contacts, Nodes, and packets;
- truthful ACK, PATH, Ping, route, signal, radio, and identity behavior;
- controlled repeater/room administration and user-authorized terminal actions;
- configured location, Wi-Fi, Map download/cache behavior, and attribution;
- conditional SD-primary retained history with visible live-only operation and
  no default NVS history fallback when required media is unavailable; and
- current release diagnostics and opt-in observer/MQTT behavior.

The exact included, conditional, and deferred product contract is
[`docs/RC1_SCOPE.md`](docs/RC1_SCOPE.md).

## Deferred from 1.0

BLE companion transport, signed OTA/update/recovery, richer QR/contact/channel
sharing, broad UI architecture work, and telemetry expansion are later work.

## Install and use

Download the published `v1.0.1` package from the
[GitHub Releases page](https://github.com/n30nex/DeskOS-MeshCore/releases), extract it
fully, and follow its `START_HERE.md`. The repository copies of the
[`user guide`](docs/USER_GUIDE_D1L.md) and
[`SD preparation guide`](docs/D1L_SD_CARD_GUIDED_INSTALL.md) explain the same
product behavior. The firmware never formats an SD card.

The ZIP is the recommended download because it presents two explicit paths:
an app BIN update for an existing DeskOS device, and a full clean 8 MB BIN for
a blank or non-DeskOS device. The same complete RP2040 UF2 is used with either
path. Standalone BIN and UF2 assets are also published for experienced
installers.
Maintainer publication steps are in the
[1.0 release runbook](docs/RC1_RELEASE_EXECUTION_D1L.md); end users should use
`START_HERE.md` from the downloaded package instead.
