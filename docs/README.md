# MeshCore DeskOS D1L Documentation

The active production candidate is MeshCore DeskOS D1L 1.0 / RC1 with
immutable profile `core_1_0` and `conditional` SD history. The agreed
user-facing surface is present in source; public release remains fail-closed
until one exact GitHub Actions package is downloaded, checksum/provenance
verified, flashed to the Pi 5 D1L, and passes the bounded physical release
gate. No soak is required for RC1.

## Active release documents

- [Public project overview](../README.md)
- [DeskOS 1.0 / RC1 user guide](USER_GUIDE_D1L.md)
- [Install/recovery history](FLASH_RECOVERY_D1L.md)
- [Historical 24-hour status and evidence ledger](release/24H_STATUS.md)
- [Current RC1 product contract](release/SIGUI_CORE_1_0_PRODUCT_CONTRACT_2026-07-18.md)
- [Fast release workflow](FAST_RELEASE_WORKFLOW_D1L.md)
- [Build decision](D1L_BUILD_DECISION.md)
- [Attributions](ATTRIBUTIONS.md)
- [Source audit and attribution](SOURCE_AUDIT_AND_ATTRIBUTION.md)

The exact GitHub Actions package and RC1 audit determine release readiness.
Historical completion ledgers, screenshots, simulator output, predecessor
hardware evidence, and Core sprint plans do not qualify the current candidate.

## Current hardware route

The release-closing D1L is currently on Raspberry Pi 5 host `neopi5`. Hardware
work uses the unprivileged, key-only `siguidev` account and the stable selector
`/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`, with USB VID:PID
`1A86:7523`. The current `/dev/ttyUSB2` resolution is observational only and
  must not be used in release commands. Stale Windows COM assignments are not
  release targets.

This routing change is not release evidence. Exact-SHA Actions and package
identity, non-erasing flash, UI/manual review, reboot and retained-state proof,
  protocol-time migration, controlled RF/DM/admin, install review, and final
  audit remain fail-closed. Narrow controlled-peer access for `siguidev` is
  still required before RF/admin evidence can qualify.

## Current contract and historical ledgers

The dated Core 1.0 contract is maintained as the current RC1 product contract.
Older completion percentages, Windows-port instructions, soak campaigns and
pre-RC1 capability matrices elsewhere under `docs/release/` are historical
evidence only.

## Safety summary

- Firmware builds run only in GitHub Actions.
- The current D1L endpoint is the exact `neopi5` stable by-id link above;
  require USB identity `1A86:7523` and never substitute a raw `/dev/ttyUSB*`.
- Do not enumerate or probe other Pi serial devices. Historical COM labels are
  evidence labels only, not current routing instructions.
- DeskOS and its validation tools never format SD.
- Release automation never transmits on the default Public channel.
