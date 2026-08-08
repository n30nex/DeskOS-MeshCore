# MeshCore DeskOS for SenseCAP Indicator D1L

DeskOS **1.2 / RC2** is the current corrective `core_1_0` release for the
SenseCAP Indicator D1L. Download the public `v1.2.0` files from the
[GitHub release](https://github.com/n30nex/DeskOS-MeshCore/releases/tag/v1.2.0).

DeskOS is a standalone, dark, touch-first MeshCore client. Public binaries are
built and packaged by GitHub Actions. GitHub Releases provides the complete
user package, standalone ESP32 BIN files, the RP2040 UF2, checksums, and
installation instructions.

## Release train

| Release | Purpose | State |
|---|---|---|
| **1.0 / RC1** | Published baseline, including the `v1.0.1` packaging correction | Shipped/historical |
| **1.2 / RC2** | Correct channel selection and Contacts, document mobile-to-D1L parity, add actual-device screenshots, and retain explicit update/fresh-install paths | Current public release (`v1.2.0`) |
| **1.5 / RC3** | Fold deferred features, architecture work, localization, and technical debt into the corrected product | Planned after RC2 |

## 1.2 / RC2 product

RC2 includes the complete 1.0 product plus:

- selecting Public or another enabled channel immediately opens its chat;
- Contacts search across name, role, fingerprint, and key;
- Recent, A-Z, Role, and Signal contact sorting;
- direct Message and Manage actions for companion DMs and repeater/room detail,
  status, and login; and
- a read-only production framebuffer export for support and actual-device
  documentation.

The retained product includes Home and core navigation; Public/channel and
direct messaging; contacts, Nodes, Finder, PATH/Ping/TRACE; repeater/room
administration; Map/location; Wi-Fi and radio/device settings; diagnostics;
and opt-in Observer/MQTT. It uses conditional SD-primary retained history with
visible live-only fallback.

If prepared SD storage is unavailable, DeskOS does not silently redirect
retained history to default NVS.

The exact product boundary is [`docs/RC2_SCOPE.md`](docs/RC2_SCOPE.md). The
current mobile-to-D1L outcome matrix is
[`docs/DESKOS_MESHCORE_FEATURE_PARITY.md`](docs/DESKOS_MESHCORE_FEATURE_PARITY.md).

## RC2 correction status

- The [#320](https://github.com/n30nex/DeskOS-MeshCore/issues/320)
  channel-selection root cause is fixed in 1.2.
- The [#321](https://github.com/n30nex/DeskOS-MeshCore/issues/321) Contacts
  search/sort/action workflows are included in 1.2.
- The full current mobile baseline and explicit D1L adaptations are recorded
  under [#322](https://github.com/n30nex/DeskOS-MeshCore/issues/322).
- The exact Actions package, attached-device update, actual-device screenshots,
  update BIN, full-clean BIN, and RP2040 UF2 are published with `v1.2.0`.

## Actual-device screenshots

These are native 480x480 production framebuffer captures from the attached D1L
running DeskOS `1.2.0`, build
`e15aff9eed9feb94bef6a81f90d62ac0f9fd9610`. The Map image was captured only
after all 9 local SD tiles were loaded and rendered. Full capture provenance is
recorded in
[`docs/screenshots/DEVICE_1_2_CAPTURE.md`](docs/screenshots/DEVICE_1_2_CAPTURE.md).

| Home | Channels |
|---|---|
| ![DeskOS 1.2 Home on the attached D1L](docs/screenshots/device-1.2-home.png) | ![DeskOS 1.2 Channels with Public on the attached D1L](docs/screenshots/device-1.2-channels.png) |

| Contacts | Settings |
|---|---|
| ![DeskOS 1.2 Contacts search, sort, and actions on the attached D1L](docs/screenshots/device-1.2-contacts.png) | ![DeskOS 1.2 Settings on the attached D1L](docs/screenshots/device-1.2-settings.png) |

![DeskOS 1.2 Map with all local SD tiles loaded on the attached D1L](docs/screenshots/device-1.2-map-local-tiles.png)

Locations and public node labels are intentionally visible. Private messages,
passwords, private keys, and admin credentials are not shown.

## Deferred to 1.5 / RC3

BLE companion transport, signed OTA/update/recovery, richer QR/deep-link
sharing, localization expansion, broad UI architecture work, and optional
telemetry expansion follow RC2. They are tracked in
[`docs/RC3_BACKLOG.md`](docs/RC3_BACKLOG.md).

## Install and use

Download the `v1.2.0` package from the
[GitHub release](https://github.com/n30nex/DeskOS-MeshCore/releases/tag/v1.2.0),
extract it fully, and follow its `START_HERE.md`. The repository
[`user guide`](docs/USER_GUIDE_D1L.md) describes the current release; the
[`SD preparation guide`](docs/D1L_SD_CARD_GUIDED_INSTALL.md) applies to both
lines. The firmware never formats an SD card.

Every release package has two explicit ESP32 paths:

- **Update:** the app BIN for an existing DeskOS installation; it preserves
  unrelated retained flash regions.
- **Fresh clean install:** the full 8 MB BIN at `0x0` for a blank device,
  another firmware, or an intentional clean start.

The same complete RP2040 SD-bridge UF2 is used with either ESP32 path because a
UF2 copy installs the full RP2040 image. Standalone BIN and UF2 assets are also
published for experienced installers.

The [historical 1.0 release runbook](docs/RC1_RELEASE_EXECUTION_D1L.md) is for
maintainers. End users should use `START_HERE.md` from the downloaded package.
