# Attributions

This project builds on open hardware/software work from upstream firmware projects. Keep this file with public source archives, release notes, and binary firmware releases.

MeshCore DeskOS D1L is released under GPL-3.0-or-later. Release packages must include the top-level `LICENSE` file with these attribution and notice documents.

## OpenStreetMap

- Map data and Standard tiles: OpenStreetMap contributors.
- Required attribution: `© OpenStreetMap contributors`.
- On-device attribution: `(c) OpenStreetMap contributors`. The display font lacks the copyright glyph, so the UI uses the ASCII `(c)` form while preserving the contributor wording.
- Copyright and attribution terms: https://www.openstreetmap.org/copyright
- Data license: Open Database License 1.0, https://opendatacommons.org/licenses/odbl/1-0/
- Tile service policy: https://operations.osmfoundation.org/policies/tiles/
- Product use: interactive Map supports zoom 8 through 18 and reuses SD-cached tiles. The built-in OpenStreetMap Standard endpoint remains visible-view-only. Background/offline area download is enabled only by an installed provider manifest whose separate service terms explicitly permit offline storage and background prefetch; the example manifest intentionally uses a non-routable placeholder and must not be pointed at OpenStreetMap Standard.
- No endorsement: OpenStreetMap and the OpenStreetMap Foundation do not endorse this project.

## LodePNG

- Project: LodePNG
- Upstream: https://lodev.org/lodepng/
- License: permissive LodePNG license; the complete notice remains in LVGL's bundled LodePNG source.
- Use in this project: `main/map/map_png_decoder.c` builds that bundled source as a private decode-only PNG decoder for map tiles.

## Seeed SenseCAP Indicator

- Project: Seeed SenseCAP Indicator ESP32 SDK and examples
- Repository: https://github.com/Seeed-Solution/SenseCAP_Indicator_ESP32
- Pinned submodule: `third_party/sensecap_indicator_esp32` at commit `77edb8d2b9a92fc67965c1b2d4a838f0d09a1800` (`v1.1.0`)
- Use in this project: tracked compatibility/BSP reference for board support, LCD/touch integration patterns, and D1L/GX display configuration. The WP-04 host-only public-group oracle also compiles `components/LoRaWAN/soft-se/aes.c`; that AES implementation carries the permissive Brian Gladman notice embedded in `aes.c` and `aes.h`. Its documented ESP-IDF v5.1.x baseline does not set the production SDK version; issue #63 separately selects v5.5.4 as the migration target and requires qualification before it becomes the production baseline.

## orlp Ed25519

- Project: orlp/ed25519
- Repository: https://github.com/orlp/ed25519
- Copyright: Copyright (c) 2015 Orson Peters <orsonpeters@gmail.com>
- License: zlib (`Zlib` in SPDX)
- Use in this project: MeshCore's pinned Ed25519 verifier remains the API and implementation base. Production and the WP-04 host gates replace only `fe.c`, `ge.c`, and `sc.c` with the plainly marked SIGUI defined-arithmetic overlay under `overlays/meshcore_ed25519_defined/`.
- Notice: the overlay retains the verbatim upstream license at `overlays/meshcore_ed25519_defined/license.txt`; release packages copy it to `notices/ORLP_ED25519_ZLIB_LICENSE.txt`.

## SigurdOS-TDeck

- Project: SigurdOS-TDeck
- Repository: https://github.com/hermes-gadget/SigurdOS-tdeck
- Upstream default branch reviewed: `dev`
- Source commit reviewed: `e7e7b12ee771edd2b15e64e1a6569b7b18f84423`
- Local follow-up reference checkout reviewed: `F:/Sigured/SigurdOS-tdeck` at `9ae0a3d03a1fcb0a6e77844ae97f0bfc5bab7f1e`; local dirty files were not copied.
- Root license file reviewed: `LICENSE`
- README license statement reviewed: `GPL-3.0-or-later`
- Bundled notice reviewed: `LICENSES/DejaVu-Fonts.txt`
- MeshCore submodule pin observed upstream: `60ea4a91bf14363e837037a79ce1bff7fa37483f`
- Upstream copyright headers reviewed: `Copyright (C) 2025 Ben`
- Upstream license headers reviewed: `GPL-3.0-or-later`
- Permission: the maintainers gave this project permission to use the work with attribution. Before public release, archive the date, channel/link, and exact scope of that permission with the release evidence.
- Use in this project: reference architecture for a production touch-first MeshCore desk firmware, especially UI shell structure, navigation patterns, cached input state, storage/map concepts, packet/log visibility, release tooling, and hardware test coverage ideas. No literal SigurdOS source is copied into this D1L slice.
- 2026-07-01 production UI pass: reviewed SigurdOS-TDeck Settings, Bluetooth, and Wi-Fi network screens as workflow references for category-style setup/status surfaces. The DeskOS D1L Wi-Fi/BLE setup sheets and packet terminal UI were implemented as original ESP-IDF/LVGL code in this repository.
- Upstream files/docs reviewed for this production slice: `docs/HOME_SCREEN.md`, `docs/MAP_SCREEN.md`, `docs/MESH_NETWORKING.md`, `docs/TERMINAL.md`, `docs/SETTINGS_SCREEN.md`, `docs/NETWORK_SCREEN.md`, `src/ui/home_screen.*`, `src/ui/navigation.*`, `src/ui/onboarding_screen.cpp`, `src/ui/screens.cpp`, `src/app/map_renderer.*`, `src/app/tile_cache.*`, `src/mesh/contact_store.*`, `src/mesh/message_store.*`, `scripts/build_metadata.py`, `scripts/merge_bin.py`, and related tests. The D1L active route probe, Wi-Fi setup, packet terminal, storage, and map work are original ESP-IDF/LVGL implementations that use those files as workflow and architecture references only.

## MeshCore T-Deck Plus LVGL Reference

- Project: MeshCore T-Deck Plus LVGL
- Repository: https://github.com/realtag-github/meshcore-tdeck-plus-lvgl
- Local reference checkout reviewed: `F:/realtag/meshcore-tdeck-plus-lvgl` at `519238f9084fe5b20e18716794a8d48a895ee6c9`
- Local license file: not present in the inspected checkout; keep this as reference-only unless a compatible license or permission archive is added.
- Use in this project: reference-only comparison for small-screen MeshCore workflows and touch-first UX constraints. No literal source is copied into this D1L slice.

SigurdOS-TDeck targets the LilyGo T-Deck GT911 touch controller, while this firmware targets the Seeed SenseCAP Indicator D1L FT5x06/GX panel. D1L touch controller code therefore stays on the Seeed BSP/FT5x06 path, with SigurdOS-TDeck used as an attributed reference for input architecture and validation behavior rather than blindly copying the GT911 driver.
