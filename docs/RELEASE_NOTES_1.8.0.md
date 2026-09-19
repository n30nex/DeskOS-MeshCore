# DeskOS D1L 1.8.0

DeskOS 1.8 brings the candidate-series improvements into the stable product
for the SenseCAP Indicator D1L. It uses the full-feature profile with prepared
SD storage for retained history and explicit live-only operation without SD.

## Everyday use

- Shared Chats, paged saved/discovered Contacts, Profile editing and six
  editable quick replies make the touch interface easier to navigate.
- Conversation drafts save to prepared SD and return after restart.
  Plain-text quote replies and the local clipboard preserve the current draft.
- Bluetooth channel sends wait for radio acceptance. A stuck radio has a
  bounded recovery path and cannot silently leave later sends blocked.
- Received phone messages report their stored hop count. Nearby channel
  messages no longer show a fixed 63-hop placeholder. Older phone-cached
  messages retain their history rather than being erased by the update.
- Contact telemetry has sufficient companion-worker stack. Nearby discovery,
  contact edits, confirmed DMs and authenticated repeater management retain
  their explicit success, timeout and permission boundaries.
- Signed local-SD updates verify the image before and after writing the
  inactive slot. The bootloader can return to the preceding image when a new
  image fails boot acceptance. USB remains the recovery path.

## Installation

Existing DeskOS devices should use the preserving update, keeping identity,
contacts, settings and SD data. Fresh installation uses the full-clean ESP32
image, complete RP2040 bridge and a prepared FAT32 card. No firmware or helper
formats the card. The package includes both USB paths, signed SD update files,
checksums and the installation guide.

## Acceptance and scope

The tagged GitHub release records the final clean source, Actions run,
artifact hashes, physical update/radio/phone results and any remaining limits.
Only observed results count as hardware acceptance. Radio telemetry and
discovery depend on compatible responding peers and their permissions.

This release does not claim complete WadaMesh parity. English-only UI,
138-byte message text, exclusive Wi-Fi/Bluetooth modes, no onboard GPS or
battery sensor, and the documented unsupported advanced phone commands remain
the product boundaries. See KNOWN_LIMITATIONS.md and the compatibility guide.
