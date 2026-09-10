# DeskOS D1L 1.8.0-rc.6

This corrects two defects reproduced with official MeshCore Android 1.49.0
on the published RC5 firmware: nearby Public messages displayed as 63 hops,
and requesting ordinary-contact telemetry restarted the D1L.

## Changes

- Incoming channel frames carry the retained hop count and path hash width
  instead of a fixed `0xFF` marker. Both old and current companion formats
  preserve zero-hop, one-hop and multi-hop paths. Multi-hop received DMs also
  retain their path metadata; zero-hop DMs keep the direct marker.
- The companion worker stack grows from 6 KiB to 12 KiB to cover identity
  validation and encrypted telemetry requests. This matches the radio owner
  budget. `ble status` exposes `protocol_task_stack_free_bytes` for checking
  its measured remaining margin on the device.
- Existing RC5 radio recovery, confirmed channel sends, contact persistence,
  discovery and signed updates are retained. The RP2040 bridge is unchanged.

## Installation and acceptance

Use the preserving update for an existing DeskOS device. Identity, settings,
contacts, drafts and SD history remain intact. The update does not erase or
rewrite messages already cached by the phone; their old hop labels may remain.
Newly received messages use the corrected path metadata.

The GitHub release records exact source/build hashes and observed physical
results. Native frame checks exercise the production encoders with legacy
and current formats, retained rows, and one/two/three-byte path hashes.
Phone telemetry acceptance requires a repeated request without a reboot and
positive measured worker stack headroom. A remote node must still permit and
answer telemetry; absence of a response is not successful sensor validation.

This remains a release candidate. The existing documented limits on advanced
phone commands, WadaMesh parity, signed-SD physical install/rollback and bridge
reflash acceptance remain in effect unless the tagged release records new
evidence. The maintainer re-enabled GitHub Actions for this candidate. The
existing workflow builds and signs the exact release package; physical
installation and radio checks use the Pi-attached D1L.
