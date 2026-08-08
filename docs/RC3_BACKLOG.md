# DeskOS 1.5 / RC3 backlog

This backlog begins only after the 1.2 / RC2 corrective parity release closes
issue #322. None of these items is part of the shipped 1.0 / RC1 product or the
1.2 / RC2 publication gate. A reproducible current defect or required
Android/iOS parity gap belongs in RC2, not here.

## BLE companion completion

Tracked by issue #324. Review the useful parts of closed draft PR #199 only
after RC2; do not revive obsolete scaffolding by default.

## Signed update, rollback, and recovery

Tracked by issue #21. Design signed OTA/SD update, rollback, and on-device
recovery as normal product features.

## Advanced QR/contact/channel sharing

Tracked by issue #20. Add richer sharing and related product capabilities
without reopening an RC2 parity defect.

## UI module and debt work

Tracked by issue #6. Take broad architecture or modularization work after the
corrected RC2 product exists.

## Telemetry and diagnostic expansion

Tracked by issue #18. Consider heartbeat, richer telemetry, crash reporting,
and diagnostic exports beyond the corrected release surface.

## Finder, Ping and Trace polish

Tracked by issue #19. Keep only genuinely optional polish here; any missing
mobile-parity workflow stays in RC2.

## Settings and preference schema

Tracked by issue #22. Consolidate preference ownership only after the working
RC2 settings surface is complete.

## Developer-only quality architecture

Tracked by issue #17. Any test-framework or longer-run development tooling must
remain outside public firmware and packages; it is not a release campaign.

## Documentation structure debt

Tracked by issue #23. Documentation architecture and developer-only report
structure remain debt unless a bounded correction is needed to ship the
working product.
