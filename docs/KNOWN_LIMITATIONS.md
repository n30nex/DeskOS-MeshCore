# Current DeskOS D1L 1.0 / RC1 limitations

These are current user-visible limits of the `core_1_0` profile.

- Selecting **#Public** can show a `channels queued` toast without opening the
  chat. This is defect #320 and is a 1.2 / RC2 release blocker.
- **Contacts** does not yet provide the mobile-app search, sorting and direct
  selected-node actions needed to inspect status, log in to a repeater, or
  start a companion DM cleanly. This is defect #321 and is a 1.2 / RC2 release
  blocker.
- Overall Android/iOS form-and-function parity remains open under #322 for 1.2
  / RC2. The presence of a source workflow is not parity acceptance.

- The D1L has no onboard GPS. Map centering and location-dependent features
  require a configured location or supported signed location data. Unknown or
  stale provenance is shown as unavailable rather than guessed.
- SD history is `conditional`. A prepared FAT32 card and paired RP2040 bridge
  provide the primary retained-history and Map-cache path. Without them the UI
  visibly remains live-only, without silent default-NVS history fallback. The
  firmware never formats it.
- Fresh Map download requires configured location, working user Wi-Fi, and an
  HTTPS provider manifest that explicitly permits offline storage and
  background prefetch. OpenStreetMap Standard is not a bulk/offline provider.
- BLE companion transport is deferred to 1.5 / RC3.
- Signed OTA, signed SD update, rollback, and on-device recovery are deferred.
  1.0 installation and recovery are host-side USB workflows.
- QR/contact/channel sharing polish is deferred to 1.5 / RC3.
- Observer/MQTT behavior is opt-in and is not enabled silently.
- Time and age labels remain unavailable until the device has a trusted time
  source.
