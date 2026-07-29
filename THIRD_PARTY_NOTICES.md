# Third Party Notices

Keep this notice with source archives and public firmware release artifacts.

## SigurdOS-TDeck

Portions of this project's architecture, touch-input behavior, and validation approach are informed by SigurdOS-TDeck.

- Copyright: Copyright (C) 2025 Ben
- License: GPL-3.0-or-later
- Source: https://github.com/hermes-gadget/SigurdOS-tdeck
- Source commit reviewed: `e7e7b12ee771edd2b15e64e1a6569b7b18f84423`
- Upstream metadata reviewed: default branch `dev`, root `LICENSE`, README license statement, `LICENSES/DejaVu-Fonts.txt`, and MeshCore submodule pin `60ea4a91bf14363e837037a79ce1bff7fa37483f`
- Permission: used with maintainer permission and attribution.

MeshCore DeskOS D1L is informed by and may adapt concepts from SigurdOS T-Deck by hermes-gadget, GPL-3.0-or-later, reviewed at commit `e7e7b12ee771edd2b15e64e1a6569b7b18f84423`: https://github.com/hermes-gadget/SigurdOS-tdeck. SigurdOS T-Deck builds on MeshCore by meshcore-dev, MIT, reviewed as submodule commit `60ea4a91bf14363e837037a79ce1bff7fa37483f`: https://github.com/meshcore-dev/MeshCore.

The reviewed upstream implementation targets LilyGo T-Deck hardware. MeshCore DeskOS D1L keeps the D1L touch path on the Seeed SenseCAP Indicator FT5x06/GX BSP driver and uses SigurdOS-TDeck as an attributed reference for product architecture, navigation, storage/map concepts, packet visibility, and validation behavior rather than copying device-specific code.

## Seeed SenseCAP Indicator

- Source: https://github.com/Seeed-Solution/sensecap_indicator_esp32
- License: Apache-2.0
- Pinned submodule commit: `77edb8d2b9a92fc67965c1b2d4a838f0d09a1800` (`v1.1.0`)
- Use: tracked compatibility/BSP reference for Seeed SenseCAP Indicator D1L display, touch, radio, and board support. Its ESP-IDF v5.1.x example baseline does not determine the separately selected v5.5.4 migration target.

## MeshCore

- Source: https://github.com/meshcore-dev/MeshCore
- License: MIT-style upstream license text
- Use: MeshCore protocol/library base and feature compatibility target.

## orlp Ed25519

- Source: https://github.com/orlp/ed25519
- Copyright: Copyright (c) 2015 Orson Peters <orsonpeters@gmail.com>
- License: zlib (`Zlib` in SPDX)
- Upstream copy: the Ed25519 implementation vendored by the MeshCore gitlink at `e8d3c53ba1ea863937081cd0caad759b832f3028`
- Altered source: `overlays/meshcore_ed25519_defined/fe.c`, `ge.c`, and `sc.c` replace undefined signed left shifts with checked-in, equivalent multiplication by powers of two. The files are plainly marked as altered and pin their upstream hashes.
- Notice: the verbatim license is retained at `overlays/meshcore_ed25519_defined/license.txt` and packaged as `notices/ORLP_ED25519_ZLIB_LICENSE.txt`.

## Brian Gladman AES implementation

- Copyright: Copyright (c) 1998-2008, Brian Gladman, Worcester, UK. All rights reserved.
- Source in this project: `third_party/sensecap_indicator_esp32/components/LoRaWAN/soft-se/aes.c` and `aes.h`
- Use: compatibility reference through the included Seeed board-support source.

Redistribution and use of this software, with or without changes, is allowed
without payment of fees or royalties provided that:

1. source distributions include the copyright notice, this list of conditions,
   and the following disclaimer;
2. binary distributions include the copyright notice, this list of conditions,
   and the following disclaimer in their documentation; and
3. the copyright holder's name is not used to endorse products built with this
   software without specific written permission.

This software is provided "as is" with no explicit or implied warranties in
respect of its properties, including, but not limited to, correctness and/or
fitness for purpose.

## OpenStreetMap Standard Tiles

- Service/data: OpenStreetMap Standard tile layer and OpenStreetMap geographic data.
- Required attribution: `© OpenStreetMap contributors`.
- Copyright notice shown in the product: `(c) OpenStreetMap contributors` (ASCII rendering used because the hardware font lacks the copyright glyph).
- Copyright and attribution terms: https://www.openstreetmap.org/copyright
- Open Database License 1.0: https://opendatacommons.org/licenses/odbl/1-0/
- Tile service policy: https://operations.osmfoundation.org/policies/tiles/
- Use: built-in current-view map tiles. The firmware may request at most the visible current-view 3x3 at one zoom only while the actual Map is visible, and reuses cached tiles. It does not offer background, multi-zoom, arbitrary-coordinate, or area download.
- Independence: OpenStreetMap and the OpenStreetMap Foundation do not endorse, sponsor, or certify MeshCore DeskOS D1L.

## LodePNG

- Upstream: https://lodev.org/lodepng/
- License: permissive LodePNG license; the license notice is retained in LVGL's bundled source.
- Use: `main/map/map_png_decoder.c` compiles LVGL's bundled LodePNG as a private decode-only PNG decoder for map tiles.
