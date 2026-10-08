# DeskOS D1L 1.9.2 limitations

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
- BLE companion and Wi-Fi are separate operating modes. Selecting either Home
  status icon safely stops the other stack first. Observer and fresh Map tile
  downloads pause in BLE mode; cached maps and RF messaging remain available.
- QR export is deliberately limited to supported public contact and channel
  material. It is not a general QR generator and never exports secrets.
- Signed update is local-SD only. It does not download firmware or accept an
  RF-triggered update. USB app/full-clean flashing remains the recovery path.
- Message text is limited to 138 UTF-8 bytes. A phone client may display a
  larger allowance for a short node name; over-limit text is rejected without
  truncation. Public/channel transmission does not acknowledge receipt at
  every recipient. Check the conversation before manually resending an
  uncertain transmission.
- The current UI is English-only. Additional localization remains future work.
- The D1L is externally powered and has no battery sensor. The phone protocol
  uses a full-battery equivalent because it has no wired-power indicator.
  Local SNR has the board driver's whole-dB resolution; airtime is calculated
  from received/completed frame lengths and the active radio settings.
- Observer/MQTT is opt-in and is never enabled silently.
- New messages use a plausible sender timestamp or the trusted local arrival
  time. Older retained rows without either remain labelled `time unknown`.
- Optional Indicator temperature, humidity, and CO2 sensor integration remains
  future work and is not represented as live data in 1.9.2.
- Packet searches load in the background. Rare/no-match searches can still
  take time on SD; pages show a lower-bound range rather than an exact total.

See [`DESKOS_MESHCORE_FEATURE_PARITY.md`](DESKOS_MESHCORE_FEATURE_PARITY.md)
for the complete mobile-to-D1L outcome matrix.

WadaMesh is the current interface comparison target, not a claim of complete
feature parity. Extra languages, full UI scaling and the Lua/web/remote app suite are not implemented.
Text sizing changes body text and inputs, with fixed heading/map-label sizes.
Mentions use name-based plain text; they do not promise a recipient notification.
Auto-add policies never overwrite saved contacts. Automatic daylight saving
supports the contemporary US/Canada and Europe/UK rules; other locations use
manual offsets. Quotes are
editable plain-text excerpts, without structured cross-client quote identifiers.
Drafts use prepared SD and remain session-only without it; the 72-entry limit
never silently evicts another draft. Clipboard text stays on this D1L and
clears when locked or restarted. See the pinned WadaMesh section of the existing parity
record for each area. Exact hardware acceptance and any remaining test limits belong in the tagged
release record. A stable release does not claim complete WadaMesh parity or
sensor responses that were not observed.
