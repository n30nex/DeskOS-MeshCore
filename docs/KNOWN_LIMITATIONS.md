# DeskOS D1L 1.6 limitations

The RC1 channel dead-end (#320) and Contacts navigation gap (#321) are fixed in
the 1.2 implementation. These are the remaining intentional product limits:

These limits apply to the production `full_feature` profile with `conditional`
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
- BLE and Wi-Fi are deliberate alternative connectivity modes rather than
  concurrent network stacks. MeshCore RF remains available in either mode.
- QR export is deliberately limited to supported public contact and channel
  material. It is not a general QR generator and never exports secrets.
- Signed update is local-SD only. It does not download firmware or accept an
  RF-triggered update. USB app/full-clean flashing remains the recovery path.
- The current UI is English-only. Additional localization remains future work.
- Observer/MQTT is opt-in and is never enabled silently.
- Time and age labels remain unavailable until the device has a trusted time
  source.

See [`DESKOS_MESHCORE_FEATURE_PARITY.md`](DESKOS_MESHCORE_FEATURE_PARITY.md)
for the complete mobile-to-D1L outcome matrix.
