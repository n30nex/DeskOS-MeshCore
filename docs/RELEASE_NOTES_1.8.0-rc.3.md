# DeskOS D1L 1.8.0-rc.3

This interface candidate uses the maintainer-selected WadaMesh workflow as its
comparison target while retaining the D1L hardware, identity, storage and radio
boundaries. It includes the earlier signed-update and phone-telemetry fixes.

## Everyday controls

- Chats combines configured channels and recent DM conversations on one landing
  page, with unread and delivery information. The dedicated DMs list pages
  through all conversations in the bounded retained store instead of stopping
  after the first five, without increasing the resident preview size.
- Contacts separates Saved and Discovered, filters by role/favourite, and pages
  through the entire matching store. A full saved-contact page no longer hides
  discovered nodes. Heard totals agree with Home.
- Profile exposes the node name, public identity, location and explicit advert
  entry after onboarding. Editing a name preserves the existing identity.
- Six saved quick replies insert at the current cursor without replacing the
  draft or sending automatically. Edits validate UTF-8 and byte limits, report
  save failures, survive preserving updates, and participate in factory reset.
- Device controls lead Settings, with a Tools shortcut for diagnostics. About opens diagnostics; offline maps say
  Offline cache when the cache is ready. Keyboards and input fields use the
  DeskOS dark colours.
- Display timeout supports 30s/1m/2m/5m/10m/Off, and local clock offsets move
  in 15-minute steps. The default remains 10 minutes; existing settings stay set.

## Parity and validation

Hardware acceptance also exposed an Observer shutdown race: an MQTT TLS
connection could still own receive buffers when Wi-Fi was deinitialized during
a switch to Bluetooth. The network shutdown now waits for all owned MQTT
clients to close, and client startup/publication rechecks the live network
under its ownership lock. A failed drain keeps the driver alive and reports
the failure. Observer settings and queued data remain intact.

USB responses are also assembled as bounded JSONL records before a single
stdio write. Wi-Fi/MQTT log lines therefore stay outside the reply instead of
splitting a quoted field and causing the flasher or support tools to time out.
The writer keeps formatting work outside the stdout lock, preserves escaped
text, and reports oversized/unavailable output without suggesting an automatic
repeat of a potentially completed command.

The [parity record](DESKOS_MESHCORE_FEATURE_PARITY.md) pins WadaMesh source
`8e94e250366632ee2e7a7198551e551aaa8b8024`, covers its primary UI areas and lists
the remaining differences. Complete WadaMesh feature parity is not claimed.
No WadaMesh source or assets were copied into DeskOS.

Native checks cover full node pagination, role filtering, boundary offsets,
reply persistence/validation, failed saves, and old completed-reset preservation.
Production LVGL renderers are also exercised on the Pi with synthetic UI data;
these images are not physical-device captures. Exact build and D1L acceptance
results belong in the tagged release record.

The phone was released to its owner after the 1.8.0-rc.2 telemetry check. No new
phone run is claimed. Physical signed-SD installation/rollback and an RP2040
reflash remain unverified; earlier radio TRACE attempts had no response.

## Install

Use the preserving installer for an existing DeskOS device. The full 8 MB
image is an explicit clean installation that replaces ESP32 identity and
settings. The signed SD files go in `deskos/updates` on the prepared FAT32 card.
The firmware never formats the card. Builds run locally on the Pi with the
pinned tools and existing signer. Stable 1.7.12 remains the stable fallback.
