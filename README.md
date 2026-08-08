# MeshCore DeskOS for SenseCAP Indicator D1L

DeskOS **1.0 / RC1** is the current production `core_1_0` firmware for the
SenseCAP Indicator D1L. GitHub Releases provides the complete user package,
standalone ESP32 BIN files, the RP2040 UF2, checksums, and installation
instructions.

DeskOS is the on-device MeshCore interface for the SenseCAP Indicator D1L. The
public 1.0 binaries use the bounded `core_1_0` profile built and packaged by
GitHub Actions.

## Release train

| Release | Purpose | State |
|---|---|---|
| **1.0 / RC1** | Current public baseline, including the `v1.0.1` packaging correction | Shipped |
| **1.2 / RC2** | Correct every current product defect and achieve MeshCore Android/iOS form-and-function parity on the D1L | In progress: [#322](https://github.com/n30nex/DeskOS-MeshCore/issues/322) |
| **1.5 / RC3** | Fold deferred features, architecture work and technical debt into the corrected product | Planned after RC2 |

## 1.0 / RC1 feature inventory

These surfaces are present in the shipped product, but this list is not a
claim of complete MeshCore mobile-app parity:

- touch-first Home and core navigation;
- Public and channel messaging, direct messages, contacts, Nodes, and packets;
- truthful ACK, PATH, Ping, route, signal, radio, and identity behavior;
- controlled repeater/room administration and user-authorized terminal actions;
- configured location, Wi-Fi, Map download/cache behavior, and attribution;
- conditional SD-primary retained history with visible live-only operation and
  no default NVS history fallback when required media is unavailable; and
- current release diagnostics and opt-in observer/MQTT behavior.

The exact included, conditional, and deferred product contract is
[`docs/RC1_SCOPE.md`](docs/RC1_SCOPE.md). The live parity ledger is
[`docs/DESKOS_MESHCORE_FEATURE_PARITY.md`](docs/DESKOS_MESHCORE_FEATURE_PARITY.md).

## Known 1.0 / RC1 issues

- Selecting **#Public** can show `channels queued` without opening the chat;
  tracked by [#320](https://github.com/n30nex/DeskOS-MeshCore/issues/320).
- **Contacts** lacks the mobile-app search, sorting and direct selected-node
  actions; tracked by [#321](https://github.com/n30nex/DeskOS-MeshCore/issues/321).
- The wider navigation and functional parity correction is tracked by
  [#322](https://github.com/n30nex/DeskOS-MeshCore/issues/322) for 1.2 / RC2.

## Actual-device screenshots

Fresh 480x480 screenshots from an actual D1L running the exact production
candidate are a **1.2 / RC2 release requirement**, tracked by
[#323](https://github.com/n30nex/DeskOS-MeshCore/issues/323). The shipped 1.0
production firmware does not expose its framebuffer over serial because its
qualification hooks are disabled. Simulator renders are therefore not shown
here as if they were device captures. RC2 must add a read-only production
export and replace this notice with current Home, Channels, Contacts, Map and
Settings images captured over the verified serial route.

## Deferred to 1.5 / RC3

BLE companion transport, signed OTA/update/recovery, richer QR/contact/channel
sharing, broad UI architecture work, and telemetry expansion follow the 1.2 /
RC2 corrective release. They are tracked in
[`docs/RC3_BACKLOG.md`](docs/RC3_BACKLOG.md).

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
