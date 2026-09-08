# DeskOS D1L 1.8.0-rc.4

This candidate makes it easier to leave a conversation and return without
losing unsent text. It retains the RC3 connection shutdown and USB reply fixes.

## Messaging

- Channel and direct-message drafts survive closing/reopening the composer.
  The existing persistence worker saves them to prepared SD after typing
  pauses. The UI reports pending saves, failures and session-only operation
  when SD is unavailable. No private draft data falls back to default NVS.
- Drafts bind to the local D1L public identity and exact channel history or
  full DM public key. A different node identity cannot restore them, including
  after a factory reset on an older rollback image. The bounded 72-entry store
  reports exhaustion without silently replacing another draft.
- Quote reply adds a shortened, editable plain-text excerpt before the draft.
  Copy/Paste uses an on-device clipboard, inserts at the cursor, and never
  transmits. The clipboard clears on lock/restart and is never persisted.
- The 138-byte wire limit and explicit Send remain unchanged. Failed sends
  retain the draft; an accepted send clears it. SD saves are asynchronous, so
  interrupted or failed writes can leave an older draft on the card. Check
  the conversation before resending after recovery.
- Message actions retain the controller's existing generation checks. Copy
  remains available for retained messages whose contact was removed; starting
  a quoted DM requires a current valid contact.

## Validation and installation

Native tests exercise production storage, UTF-8 handling, media-generation
changes, failed/interrupted writes, deletion tombstones, ownership and bounded
capacity. Production LVGL callbacks exercise closing/reopening, target
isolation, cursor paste, quoting, failed Send and successful draft clearing.
Exact full-suite, firmware, device and phone results belong in the tagged
release record; simulation is not physical-device acceptance.

Builds run locally on the Pi with pinned ESP-IDF 5.5.4 source and official
ESP32-S3 tools. Use the preserving installer for an existing DeskOS device;
the full 8 MB image is explicitly destructive recovery. Signed update files
belong in `deskos/updates` on an already prepared FAT32 card. The production
RP2040 image is unchanged. No path formats the card.

The remaining WadaMesh differences are listed in
[the parity record](DESKOS_MESHCORE_FEATURE_PARITY.md). Stable 1.7.12 remains
available; this version is a release candidate.
