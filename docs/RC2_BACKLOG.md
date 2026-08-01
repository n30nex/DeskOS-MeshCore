# DeskOS RC2 backlog

This is the only future-work backlog. None of these items is an RC1 gate.

## BLE companion completion

Review and rebase the existing draft PR only after RC1; do not merge it during
RC1 closure.

## Signed update, rollback, and recovery

Design and qualify signed OTA/SD update, rollback, and on-device recovery.

## Advanced QR/contact/channel sharing

Add richer sharing, contact paging, channel management, and related polish.

## UI module and debt work

Take broad architecture or modularization work only when it is not being used
to expand an RC1 defect fix.

## Telemetry and diagnostic expansion

Consider heartbeat, richer telemetry, crash reporting, and diagnostic exports
beyond the current release surface.

## Optional UX enhancements

Track non-blocking navigation, visual, Finder, Ping, Trace, and settings polish.

## Longer reliability qualification

Run post-RC1 endurance and traffic campaigns as product-development evidence,
not retroactive RC1 gates.

## Technical debt moved from RC1

Track test-framework expansion, preference/schema discipline, and public-doc
structure improvements here unless a narrow reproducible defect requires a
bounded fix.
