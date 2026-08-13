# DeskOS MeshCore 1.7.1

DeskOS 1.7.1 repairs repeater and room administration on the SenseCAP
Indicator D1L.

## Highlights

- Direct **Login** actions beside managed contacts and in contact detail.
- Large masked password entry with a full on-screen keyboard.
- Optional per-server password saving on the D1L, with explicit Forget,
  contact-deletion cleanup, and factory-reset cleanup.
- Dedicated management dashboard for status, telemetry, neighbours, access,
  tools, room posts, and authenticated console commands.
- Animated pending state, timeout guidance, cancellation, and persistent
  result/error feedback for server operations.
- Existing permission checks, redaction, confirmation, and RF safety remain in
  force.

## Validation

- Native credential-store behavior and failure handling.
- Direct contact and node-detail navigation contracts.
- 105 rendered 480x480 views with no overflow, truncation, overlap, or touch
  target failure.
- 1,000 UI lifecycle transitions.
- Exact GitHub Actions build and physical D1L results are recorded with the
  published release.

Issue: [#336](https://github.com/n30nex/DeskOS-MeshCore/issues/336)
