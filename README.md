# MeshCore DeskOS for SenseCAP Indicator D1L

Current status: DeskOS 1.0 RC1 candidate under final exact-package acceptance.
No stable v1.0.0 release is claimed until the RC1 audit reports
`ready_for_public_release=true`.

SIGUI is the on-device MeshCore DeskOS interface for the SenseCAP Indicator
D1L. RC1 is the bounded `core_1_0` profile built and packaged by GitHub Actions.

## RC1 feature surface

- touch-first Home and core navigation;
- Public and channel messaging, direct messages, contacts, Nodes, and packets;
- truthful ACK, PATH, Ping, route, signal, radio, and identity behavior;
- controlled repeater/room administration and user-authorized terminal actions;
- configured location, Wi-Fi, Map download/cache behavior, and attribution;
- conditional SD-primary retained history with visible live-only operation and
  no default NVS history fallback when required media is unavailable; and
- current release diagnostics and opt-in observer/MQTT behavior.

The exact included, conditional, and deferred contract is
[`docs/RC1_SCOPE.md`](docs/RC1_SCOPE.md).

## Deferred from RC1

BLE companion transport, signed OTA/update/recovery, richer QR/contact/channel
sharing, broad UI architecture work, and telemetry expansion are RC2 work.
They are not hidden RC1 release gates.

## Install and use

Ordinary users should download the published `v1.0.0-rc.1` package from the
[GitHub Releases page](https://github.com/n30nex/SIGUI/releases), extract it
fully, and follow its `START_HERE.md`. The repository copies of the
[`user guide`](docs/USER_GUIDE_D1L.md) and
[`SD preparation guide`](docs/D1L_SD_CARD_GUIDED_INSTALL.md) explain the same
product behavior. The firmware never formats an SD card.

Maintainer final-candidate acceptance is a separate, bounded procedure. It
uses one exact successful `main`-push package, one non-erasing flash, four
machine evidence sources, and
[`scripts/rc1_release_gate_audit_d1l.py`](scripts/rc1_release_gate_audit_d1l.py).
See the [RC1 release runbook](docs/RC1_RELEASE_EXECUTION_D1L.md); do not use it
as ordinary installation guidance.

Stable `v1.0.0` remains reserved for a later promotion of an accepted release
candidate.
