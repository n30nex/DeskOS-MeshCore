# DeskOS MeshCore 1.7.7

DeskOS 1.7.7 focuses on radio-to-screen responsiveness for the SenseCAP
Indicator D1L.

## Improvements

- Verified adverts are visible to Contacts immediately instead of waiting for
  an SD-card persistence operation to finish.
- Ambient contact writes are coalesced by the existing one-second retained
  storage worker, reducing repeated SD work on busy meshes.
- Heard-node queries use an efficient library sort rather than a quadratic
  full-store selection loop.
- Contact filtering, ordering, collision protection, and user preferences are
  unchanged.

## Safety

- Rename, favorite, mute, import, and other explicit user edits remain
  synchronously durable.
- Deferred advert updates remain queued after a write failure and are retried
  by the storage worker.
- Controlled reboot still forces retained storage to flush before restart.
- The update preserves identity, contacts, settings, history, radio settings,
  and SD contents.

## Validation

- Native contact-store write-failure and retry coverage.
- Native node query ordering and filtering coverage.
- Complete host suite, exact GitHub Actions artifacts, and physical D1L checks
  are recorded with the published release.
