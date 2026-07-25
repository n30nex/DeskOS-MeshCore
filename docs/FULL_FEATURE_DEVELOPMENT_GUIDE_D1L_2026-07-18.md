# MeshCore DeskOS D1L Full Feature Development Guide

> Historical development reference captured on 2026-07-18. This is not the
> MeshCore DeskOS D1L Core 1.0 user guide, is not included in the Core release
> package, and describes features that are intentionally unreachable in the
> `core_1_0` profile. See `USER_GUIDE_D1L.md` for the shipping Core surface.

This firmware turns a Seeed SenseCAP Indicator D1L into a touch-first desk console for a local MeshCore mesh.

## What Works Now

- Canada/USA MeshCore radio profile: 910.525 MHz, BW62.5, SF7, CR5, 20 dBm, TCXO NONE.
- Local MeshCore identity stored in D1L NVS.
- Public MeshCore `test` send/receive against local bots.
- Signed advert receive/transmit.
- Persisted recent Public messages, bounded Public History/Search, DM rows, heard nodes, contacts, routes, packet evidence, unread state, and reset history.
- Messages opens to the Public channel by default, with an explicit DMs mode for retained direct-message conversations, thread review, and bounded search inside the exact selected conversation. Applying, clearing, or paging a DM search does not transmit, write retained history, or advance a read cursor. The Public badge clears only when `Mark read` advances the Public cursor. Opening a DM conversation advances only that exact full-fingerprint cursor; it never clears another contact, and reopening an already-read conversation is cursor-idempotent. The DMs list shows one newest-first row per contact even when the newest row was sent by you. Muted unread is reported separately from the audible Public/DM attention count.
- Touch Public/DM compose enforces the 138-byte MeshCore UTF-8 limit and shows both decoded characters and encoded bytes. Empty, malformed, control-containing, oversized, radio-busy, invalid route-policy, or protocol-clock-unavailable input/state keeps Send disabled. DM compose also revalidates the exact contact, local identity health, and any active delivery immediately before Send. A transient failure keeps the draft and offers only an explicit user retry; it never retries or transmits from refresh/navigation.
- Tapping a Public message opens a detail sheet with sender, message text, signal, retained path evidence, and a Reply action that opens Public compose without transmitting until Send. Public packets provide only an unverified sender display name, not an authenticated full key, so `DM sender` explains `sender_name_unverified` and never alias-matches that name, opens DM compose, or transmits. A DM action opens compose only after the selected Contact or heard-node identity resolves by its complete public key to the same retained canonical chat Contact. Heard-only nodes, missing/stale contacts, incomplete or mismatched identities, and non-chat roles show an explicit reason code and remain read-only.
- Messages derives loading, ready, storage-degraded, and persistence-unavailable notices from the live Public/DM store state. Loading is not shown as an empty conversation. An SD or backup fault does not hide readable RAM history: degraded storage reports the fault while keeping current rows readable, and unavailable persistence is called out separately. With no DM rows, the list distinguishes having no verified chat Contacts from having at least one verified chat Contact but no conversation history. A selected thread remains searchable and readable if its Contact disappears, but Reply is disabled. Active bounded delivery retries and retained final failures are shown separately. Rendering, refreshing, searching, or paging these states does not transmit or advance a read cursor; opening a DM conversation intentionally marks that selected thread read. Incoming retained Public/DM events refresh the active relevant list or thread without switching the selected Messages mode, closing compose/search/thread overlays, changing a draft or query, moving keyboard focus, or automatically transmitting.
- The scoped Messages software behavior is implemented through PR #150. Full release qualification still requires the exact-candidate physical touch/keyboard/scroll/UTF-8 review, controlled-peer delivery evidence, dependency closure, and the Messages acceptance artifact. Curated emoji/glyph, notification, accessibility, and polish work is tracked separately under WP-21.
- Home is a quiet five-destination dashboard: Messages, Network, Map, and More are the task entries, while one Device card summarizes Time, Wi-Fi, Bluetooth, and SD state. Secondary tools such as Packets and diagnostics are grouped under More instead of competing on the Home screen.
- Targeted outbound DM to a local MeshCore bot has been verified through hardware counters and D1L packet/message logs.
- First-boot setup for node name, Canada/USA preset confirmation, Desk Companion role, offline radio defaults, and local identity generation.
- 480x480 dark touch shell with Home, Messages, Network, Map, More, full-height nested pages, toast feedback, onboarding, and lock overlay.
- More groups Tools, Connections, Storage & maps, Device, Support, and Advanced into simple menus. SD Card opens a quiet Storage page with separate Card status and Data locations subpages; the other leaves remain Wi-Fi, BLE, Radio, Map options, Display, Identity, Diagnostics, About, and Packets.
- Staged touch radio settings editor for frequency, bandwidth, SF, CR, TX power, RX boost, and US/CAN defaults. Saved profile changes are persisted and flagged as reboot/apply pending.
- Mesh visibility summaries for signal, room servers, and repeater candidates. `Packets -> Mesh Roles` opens a two-choice root; Rooms and Repeaters then open separate bounded read-only lists, with Back returning one level at a time.
- Map opens the actual current view with direct `-`, `+`, and `Center` controls plus one `Options` setup action. It starts at regional zoom 10, permits zooms 8 through 14, and pans with a one-finger drag; a drag commits the next view only when the finger is released. `Center` returns to the saved manual location. D1L has no onboard GPS. Coordinates received in valid signed peer adverts can appear as bright role-coloured node markers with high-contrast names below them and are described as `Advert location`, not live GPS. At most eight non-overlapping markers are shown from the 32 newest located nodes; crowded markers are omitted instead of showing an unlabeled dot. Tap within a marker's 44 px viewport hit area to open Node Detail, including its advert coordinates; Close returns to the unchanged retained Map view without intentionally downloading it again. OpenStreetMap Standard remains the built-in source and cache format; decoded tiles receive a deterministic local dark style so no second provider, account, key, or duplicate cache is needed. The setup hierarchy remains `Map -> Map options -> Set location or Cache status` (canonical probes: `map -> map_options -> map_location or map_cache`). Back from either child returns to Map options; Save location returns to the actual Map. With Wi-Fi connected, each committed view may load only its visible current-view 3x3 at one zoom per visible generation, selected by the user. A completed exact-view Home-to-Map revisit reuses the retained rendered frame without network or SD reread, while later sessions reuse cached tiles. There is no background, multi-zoom prefetch, off-screen batch, or area download; `(c) OpenStreetMap contributors` stays visible.
- Packets is now a terminal-style diagnostic feed with RX/TX/fail/error color treatment, filter/search controls, pause/resume, route evidence, and normal/advanced packet detail.
- Network shows role badges for companions, repeaters, room servers, sensors, and unknown nodes, plus a read-only heard-node detail page for role, fingerprint, public-key state, signal, path, and heard-count evidence.
- Read-only route trace helper for promoted contacts or known fingerprints, backed by retained route/contact evidence.
- Contact export for promoted contacts with retained public keys, using MeshCore-compatible `meshcore://contact/add?...` serial output and a touch QR sheet.
- USB serial diagnostics, smoke test, and soak test tooling.

## Release Package Flashing

Use a release package from GitHub Actions or `artifacts/release`.

Normal project flash writes the bootloader, partition table, and app image at ESP-IDF offsets while preserving unrelated flash regions.

```powershell
$env:D1L_PORT = "COMx"
.\flash_project.ps1 -Port $env:D1L_PORT
```

Never use COM8, COM11, or COM29 as the D1L serial/flash target unless the operator explicitly reassigns the hardware. In the current validation setup, use COM12 for the D1L; COM11 may be checked separately only as the independent bot/radio endpoint for controlled DM evidence.

The full 8MB image is for factory/recovery workflows and can overwrite persisted settings, contacts, messages, and logs. It requires typed confirmation:

```powershell
$env:D1L_PORT = "COMx"
.\flash_full_8mb.ps1 -Port $env:D1L_PORT
```

## After Flash

Run smoke from the repo checkout:

```powershell
$env:D1L_PORT = "COMx"
python .\scripts\smoke_d1l.py --port $env:D1L_PORT --manual-touch
```

For a short active stability probe, use a promoted controlled peer such as the
COM11 bot. Automated probes use direct messages and never the default Public
channel; `#test` may be used once channel selection is available:

```powershell
$env:D1L_DM_FINGERPRINT = "<16-hex-controlled-peer>"
python .\scripts\soak_d1l.py --port $env:D1L_PORT --duration-sec 180 --sample-interval-sec 45 --active-dm-fingerprint $env:D1L_DM_FINGERPRINT --active-dm-text test --active-interval-sec 60 --require-rx-delta --min-tx-delta 1
```

## Useful Serial Commands

- `version`
- `settings get`
- `settings onboarding status`
- `settings onboarding complete <name>`
- `health`
- `crashlog`
- `mesh status`
- `mesh send public test` (manual `#test`-channel validation only; automated probes use DM)
- `radio get`
- `radio set preset uscan`
- `radio set txpower 20`
- `radio set rxboost <0|1>`
- `ui capture status`
- `ui capture begin`
- `ui capture chunk <offset> <len>`
- `ui capture end`
- `ui compose-probe <public|public-long|dm|dm-long|public-search|dm-search|packet-search|contact-edit|onboarding|map-location|wifi-ssid|wifi-password>`
- `ui scroll-probe <...|mesh_roles|mesh_rooms|mesh_repeaters|storage|storage_card|storage_data|map|map_options|map_location|map_cache|...>` (read-only capture aliases; Map probes arm network suppression before opening the Map and never request map tiles)
- `messages public`
- `messages public offset 8`
- `messages public search test`
- `messages public search test offset 8`
- `messages dm offset 8`
- `messages dm <fingerprint> offset 8`
- `messages unread`
- `nodes`
- `contacts`
- `contacts export`
- `contacts export <fingerprint>`
- `contacts rename <fingerprint> <alias>`
- `contacts delete <fingerprint>`
- `routes`
- `routes trace <fingerprint>`
- `packets`
- `signal`
- `roomservers`
- `repeaters`
- `wifi status`
- `wifi on`
- `wifi scan`
- `wifi save <ssid> [password]`
- `wifi connect`
- `wifi off`
- `wifi clear`
- `ble status`
- `storage status`
- `storage setup`
- `storage map-tile-canary <token>`
- `storage export-canary <token>`
- `storage export-diagnostics <token>`
- `storage export-data <token>`
- `storage map-policy`
- `map center`
- `map center set <lat> <lon>`
- `map center clear`

## Current Limits

Retained Public/DM message history can use SD only behind the ready file-operation gate and always keeps onboard fallback.

- Manual physical review of the touch UI is still pending.
- Hardware pixel capture over the COM12 console is the primary way to prove what the D1L display drew. PR #60 / source `0b138be` passed Actions `29068006554` and `29068007961`, then captured Mesh Roles (`63DE54FB`), Rooms (`FD538D71`), and Repeaters (`5C41EE08`) with matching firmware/host CRCs, passing simulator diffs, and a clean three-round all-tab probe without Public RF or formatting. The newer Storage/Card status/Data locations hierarchy still needs its own exact Actions/COM12 captures and physical Back/scroll review.
- Manual touch review of the Settings dashboard, the new read-only Storage hierarchy, and Radio Settings is still pending.
- Manual touch review of the Contact Edit rename/Forget sheet is still pending.
- Full DM ACK/PATH, direct-route, and inbound-DM RF proof is still pending.
- Route Trace is retained local evidence only; active RF ping/trace commands are not enabled yet.
- Contact export QR scanning/import has not yet been manually proven with a phone/client.
- Wi-Fi is off by default but can be explicitly enabled for local setup, bounded `wifi scan`, saved-profile `wifi connect`, and the touch Wi-Fi Setup sheet. Map has a built-in OpenStreetMap Standard source and no source/account/key editor. Network access is limited to the visible current-view 3x3 at one zoom per visible generation, with the user selecting a level between 8 and 14 while the actual Map is active. Pan and zoom create a new bounded visible-view plan only after the drag is released or a zoom control is tapped; they do not start background or multi-zoom prefetch. BLE companion runtime, OTA, and hardware proof remain pending.
- The saved location persists. Pan and zoom are session view state: Home-to-Map keeps a completed exact view in the retained rendered frame, while a reboot starts again at the saved center and default zoom 10 and may reread tiles from the SD cache. Neither path should duplicate network requests for tiles already cached. A physically opened Map waits without tile or network activity while the complete SD file gate prepares, then resumes that same visible generation automatically; leaving or covering Map cancels the wait. The revised controls, same-view frame reuse, combined ESP-IDF v5.5.4 pixels, and physical request/render/cancel/cache flow remain release gates. Page-open probes are read-only and network-suppressed: no map tile request, no Wi-Fi mutation, no RF, and no storage mutation.
- Optional SD-card data storage is staged but not fully hardware-accepted yet. The touch path is read-only: `More -> Storage & maps -> SD Card` opens Storage, then Card status or Data locations; a fixed footer says DeskOS never formats cards, no raw setup slug is shown, and these pages expose no mount, remount, delete, or format callback. PR #61 / source `4d9f384` proves this hierarchy on exact Actions-built COM12 pixels with passing simulator diffs and moved-and-restored Data scrolling; only manual physical touch/photos remain open for the Storage UI. Current builds also expose `storage status`, `storage map-policy`, non-destructive `storage setup`, and serial-only file/history/map/export canaries, include a GitHub Actions-built RP2040 SD bridge target, and never format SD cards. Users must provide FAT32 cards prepared on a computer. When the bridge reports a ready card plus file-operation and atomic-rename support, retained Public/DM history, routes, packets, exports, and map tiles can report SD backends with onboard fallback. Settings, identity, contacts, and read-state remain onboard/fallback-backed or pending until later migrations.
- Full 12-hour idle/listening soak is still pending; the 1-hour active Public `test` soak has passed.
- Flash backup was intentionally skipped during current bring-up when the operator requested it.
