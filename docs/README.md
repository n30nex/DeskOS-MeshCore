# MeshCore DeskOS D1L Documentation

The active production candidate is MeshCore DeskOS D1L Full Feature with
immutable profile `full_feature` and `conditional` SD history. The complete
user-facing surface is present; public release remains fail-closed until one
exact Actions package passes the Pi 5 device, reboot, storage, controlled-peer,
physical UI, and 12-hour soak gates.

## Active release documents

- [Public project overview](../README.md)
- [Full Feature user guide](USER_GUIDE_D1L.md)
- [Flash and recovery](FLASH_RECOVERY_D1L.md)
- [Production status and evidence ledger](release/24H_STATUS.md)
- [Fast release workflow](FAST_RELEASE_WORKFLOW_D1L.md)
- [Build decision](D1L_BUILD_DECISION.md)
- [Attributions](ATTRIBUTIONS.md)
- [Source audit and attribution](SOURCE_AUDIT_AND_ATTRIBUTION.md)

The exact GitHub Actions package and Full Feature audit determine release readiness.
Historical completion ledgers, screenshots, simulator output, predecessor
hardware evidence, and Core sprint plans do not qualify the current candidate.

## Current hardware route

The release-closing D1L is currently on Raspberry Pi 5 host `neopi5`. Hardware
work uses the unprivileged, key-only `siguidev` account and the stable selector
`/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`, with USB VID:PID
`1A86:7523`. The current `/dev/ttyUSB2` resolution is observational only and
must not be used in release commands. `COM12` remains the valid Windows
alternative.

This routing change is not release evidence. Exact-SHA Actions and package
identity, non-erasing flash, UI/manual review, reboot and retained-state proof,
protocol-time migration, controlled RF/DM, active/idle soak, install review,
and final audit remain fail-closed. Narrow controlled-peer access for
`siguidev` is still required before RF or active-soak evidence can qualify.

## Historical Core sprint

The Core 1.0 product contract, 24-hour audit/roadmap, and execution backlog
under `docs/release/` are retained as historical recovery-sprint records. They
do not define the current Full Feature package or its production gates.

## Safety summary

- Firmware builds run only in GitHub Actions.
- The current D1L endpoint is the exact `neopi5` stable by-id link above;
  require USB identity `1A86:7523` and never substitute a raw `/dev/ttyUSB*`.
- COM12 remains the valid Windows D1L alternative.
- COM16 is reserved for separately authorized SD/RP2040 work and is never the
  Core D1L endpoint.
- COM8, COM11, and COM29 are forbidden.
- DeskOS and its validation tools never format SD.
- Release automation never transmits on the default Public channel.
