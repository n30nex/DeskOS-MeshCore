# MeshCore DeskOS for SenseCAP Indicator D1L

DeskOS **1.2 / RC2** is the corrective `core_1_0` release candidate for the
SenseCAP Indicator D1L. The latest published baseline remains **1.0 / RC1**
until the RC2 exact-device capture and public artifact steps are complete.

DeskOS is a standalone, dark, touch-first MeshCore client. Public binaries are
built and packaged by GitHub Actions. GitHub Releases provides the complete
user package, standalone ESP32 BIN files, the RP2040 UF2, checksums, and
installation instructions.

## Release train

| Release | Purpose | State |
|---|---|---|
| **1.0 / RC1** | Published baseline, including the `v1.0.1` packaging correction | Shipped/historical |
| **1.2 / RC2** | Correct channel selection and Contacts, document mobile-to-D1L parity, add actual-device screenshots, and retain explicit update/fresh-install paths | Candidate implementation complete; device capture/publication in progress under [#322](https://github.com/n30nex/DeskOS-MeshCore/issues/322) |
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
administration; Map/location; Wi-Fi and radio/device settings; conditional
SD-primary history with visible live-only fallback; diagnostics; and opt-in
Observer/MQTT.

The exact product boundary is [`docs/RC2_SCOPE.md`](docs/RC2_SCOPE.md). The
current mobile-to-D1L outcome matrix is
[`docs/DESKOS_MESHCORE_FEATURE_PARITY.md`](docs/DESKOS_MESHCORE_FEATURE_PARITY.md).

## RC2 correction status

- The [#320](https://github.com/n30nex/DeskOS-MeshCore/issues/320)
  channel-selection root cause is fixed in the RC2 candidate.
- The [#321](https://github.com/n30nex/DeskOS-MeshCore/issues/321) Contacts
  search/sort/action workflows are implemented in the RC2 candidate.
- The full current mobile baseline and explicit D1L adaptations are recorded
  under [#322](https://github.com/n30nex/DeskOS-MeshCore/issues/322).
- Exact Actions build, attached-device use, screenshots, and publication remain
  before those issues close.

## Actual-device screenshots

RC2 now contains the minimal read-only production serial framebuffer export.
Fresh 480x480 Home, Channels, Contacts, Map, and Settings images from the
attached D1L will replace this notice before publication and close
[#323](https://github.com/n30nex/DeskOS-MeshCore/issues/323). Locations may be
visible; private messages, passwords, keys, and admin credentials will not be
published.

## Deferred to 1.5 / RC3

BLE companion transport, signed OTA/update/recovery, richer QR/deep-link
sharing, localization expansion, broad UI architecture work, and optional
telemetry expansion follow RC2. They are tracked in
[`docs/RC3_BACKLOG.md`](docs/RC3_BACKLOG.md).

## Install and use

Until RC2 publication, download the published `v1.0.1` package from the
[GitHub Releases page](https://github.com/n30nex/DeskOS-MeshCore/releases),
extract it fully, and follow its `START_HERE.md`. The repository
[`user guide`](docs/USER_GUIDE_D1L.md) describes the RC2 candidate; the
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
