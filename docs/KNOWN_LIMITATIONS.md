# DeskOS D1L 1.2 / RC2 limitations

The RC1 channel dead-end (#320) and Contacts navigation gap (#321) are fixed in
the 1.2 implementation. These are the remaining intentional product limits:

These limits apply to the production `core_1_0` profile with `conditional`
SD-primary storage.

- The D1L has no onboard GPS. Map centering and location-dependent features use
  a configured location or supported signed location data. Unknown or stale
  provenance is shown as unavailable rather than guessed.
- SD history is `conditional`. A prepared FAT32 card and paired RP2040 bridge
  provide retained history and Map cache. Without them, RF chat remains visibly
  live-only and history is not silently redirected into default NVS. The
  card remains user-owned; the firmware never formats it.
- Fresh Map download also requires user-configured Wi-Fi and an HTTPS provider
  manifest that explicitly permits offline storage and background prefetch.
  OpenStreetMap Standard remains visible-current-view-only.
- BLE companion transport is deferred to 1.5/RC3; DeskOS is a standalone
  on-device MeshCore client.
- Rich on-device QR/deep-link sharing is deferred. Existing URI import and
  contact/channel management remain available.
- Signed OTA, signed SD update, rollback, and on-device recovery are deferred.
  Public 1.2 installation/recovery uses the app update BIN or full clean 8 MB
  BIN over USB plus the complete RP2040 UF2 through BOOTSEL.
- The current UI is English-only. Additional localization is RC3 work.
- Observer/MQTT is opt-in and is never enabled silently.
- Time and age labels remain unavailable until the device has a trusted time
  source.

See [`DESKOS_MESHCORE_FEATURE_PARITY.md`](DESKOS_MESHCORE_FEATURE_PARITY.md)
for the complete mobile-to-D1L outcome matrix.
