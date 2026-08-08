# DeskOS 1.2 / RC2 product scope

DeskOS 1.2/RC2 is the corrective public `core_1_0` release for the SenseCAP
Indicator D1L. The profile name remains stable because the product boundary did
not require another firmware variant.

## Included

- the complete shipped 1.0 feature set;
- channel selection that opens the selected conversation, including Public;
- mobile-style Contacts search and Recent/A-Z/Role/Signal sorting;
- direct selected-contact Message and Manage actions;
- reachable companion DM and repeater/room detail, status, and login paths;
- a checked-in Android/iOS-to-D1L parity ledger;
- read-only production framebuffer export for support and actual-device docs;
- conditional SD-primary history/Map storage with visible live-only fallback;
- an ESP32 app update BIN for existing DeskOS devices;
- a full clean 8 MB ESP32 BIN for blank/non-DeskOS devices;
- one complete RP2040 SD-bridge UF2 used for either ESP32 path; and
- checksum-bound Windows/Linux installation instructions.

## Public runtime boundary

RC2 is ordinary production firmware for use on the owner's own mesh. It does
not contain or require a controlled peer, Wi-Fi credentials, admin password,
developer UI, probe commands, qualification hooks, soak campaign, or validation
receipt. The production screenshot export only copies the framebuffer already
being displayed; it cannot transmit RF or format storage.

The device never formats an SD card. Normal setup may be offline. Map download
requires user-configured Wi-Fi, location, prepared storage, and an authorized
provider, but those are runtime choices rather than release prerequisites.

## Accepted D1L adaptations and 1.5 / RC3

The D1L is a standalone client rather than a phone accessory. It uses on-device
onboarding, on-device unread state, manual/signed location instead of phone GPS,
and host USB installation instead of a mobile app store updater. BLE companion
transport, richer QR/deep-link sharing, signed OTA/on-device recovery,
localization expansion, broad UI architecture work, and optional telemetry
growth remain in [`RC3_BACKLOG.md`](RC3_BACKLOG.md) for 1.5/RC3.

The exact user-facing parity decision for every current mobile area is in
[`DESKOS_MESHCORE_FEATURE_PARITY.md`](DESKOS_MESHCORE_FEATURE_PARITY.md).
