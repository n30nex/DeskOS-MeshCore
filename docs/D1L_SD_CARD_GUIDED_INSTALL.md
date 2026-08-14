# DeskOS SD Card Setup

DeskOS uses a prepared SD card as its persistent history and map-cache
store. Use a 32 GB or larger card formatted as FAT32. DeskOS never formats a
card.

## Prepare the Card

### Browser flasher

In Chrome or Edge, open the DeskOS onboarding step in the NeonPocket flasher
and choose **Prepare SD card**. Select the root of an already-formatted FAT32
card. The browser creates the required directories, checksum-verifies every
source and read-back file, accepts identical files, and stops rather than
replacing a different existing file.

The browser cannot and does not format the card. After preparation, insert it
in the D1L and choose **Verify in DeskOS** to confirm that the RP2040 bridge can
mount it and open the DeskOS data root.

### Release-package script

The release package contains `scripts/prepare_deskos_sd.py` and the complete
`sdcard/` payload. Run the preparer from the unpacked release:

```text
python scripts/prepare_deskos_sd.py --target <mounted-card-root>
```

That first command is a read-only plan. Review the exact target and then apply
it:

```text
python scripts/prepare_deskos_sd.py --target <mounted-card-root> --apply
```

The preparer:

- requires FAT32 and at least 28,000,000,000 bytes of card capacity;
- creates only the documented `deskos/` tree;
- never formats the card;
- refuses to overwrite a different existing file;
- verifies every copied file by SHA-256; and
- writes `deskos/card-preparation-receipt.json`.

## Included Map Provider

DeskOS includes an authorized Natural Resources Canada CBMT provider for
background prefetch and offline SD caching. The preparer installs its metadata
at `deskos/map/offline-provider.json`. Firmware also seeds the same metadata
when that file is absent on an otherwise ready DeskOS card, so an already
installed card does not need to be removed or reformatted. Neither path
replaces a different provider file already on the card.

Re-running it accepts identical files and rejects a changed destination.
`--skip-filesystem-check` is only for a non-card staging directory and must not
be used to bypass checks on real media.

## Optional Preloaded Map Tiles

Tiles may be preloaded only from a provider that explicitly permits offline
storage. The tile source must contain `offline-tile-provider.json` with
`offline_storage_permitted=true`, provider attribution, and a license URL:

```text
python scripts/prepare_deskos_sd.py --target <mounted-card-root> \
  --tiles-from <licensed-tile-directory> --apply
```

That option replaces the included provider only on a newly prepared card.
Do not bulk-download from `tile.openstreetmap.org`. The built-in OpenStreetMap
policy supports bounded current-view caching, not unattended offline-area
prefetch. A provider example is included at
`sdcard/offline-tile-provider.example.json`.

## Install and Check

1. Safely eject the prepared card and insert it while the D1L is powered off.
2. Boot DeskOS.
3. Open **Settings > Storage**.
4. Require the SD state to become ready and the retained stores to report SD as
   their persistent backend.
5. Open **Map**, allow an authorized tile request, then revisit the same view
   to confirm it loads from the cache.

If the card, FAT32 filesystem, or RP2040 bridge is unavailable, DeskOS remains
usable for live RF transmit/receive, Public chat, and direct chat. It displays
a degraded-mode notice and does not silently redirect persistent histories to
NVS. Correct the card or bridge and reboot; do not format the card on the
device.

The release package contains the production RP2040 SD bridge UF2. Hold BOOTSEL
while connecting the RP2040 USB side. The browser flasher checks the selected
drive for its RP2040 identity, checksum-verifies the exact UF2, copies it, and
then asks you to reconnect the ESP32 side for a device-level bridge check.

## Installation boundary

The production installer checksum-verifies the package before writing. On
Linux, the ESP32 target is selected only through
`/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0` with USB identity
`1a86:7523`; raw `/dev/ttyUSB*` paths are never accepted. Card setup itself
does not send RF, erase NVS, flash firmware, or format storage.
