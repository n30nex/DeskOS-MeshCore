# 480x480 Dark UI Spec

DeskOS 1.0 uses a production touch shell. Bring-up-only commands remain
available in development builds, not in customer firmware:

- 480x480 fixed layout.
- Near-black blue background, graphite tiles, cyan status accent, amber warnings, red only for errors.
- Minimum touch target 44x44 px; primary targets 56x56 px or larger.
- Status hierarchy visible from desk distance.
- No Apple assets, SF Symbols, copied screenshots, or proprietary UI assets.

## Implemented Shell Slice

- Every boot begins with a dock-independent 480x480 readiness overlay showing
  determinate five-row progress for Display, Identity, Radio, Storage & maps,
  and UI.
  Home remains covered until all essential rows are green. Prepared FAT32 SD
  and the authorized NRCan provider are reported separately and never promoted
  to ready without actual media/provider evidence. Configured users then
  continue unchanged; fresh users enter a six-step, 44px-target first-start
  Wi-Fi/offline mode, fixed Canadian radio confirmation, required prepared
  FAT32 SD/NRCan validation, and Public/#bot/#test review. Completion is blocked
  until the card and authorized provider manifest are present. Firmware never
  formats SD.
- Home keeps its compact status treatment. Every non-Home destination owns a
  compact mobile-style app bar, so the old global diagnostic header does not
  consume chat, list, settings, or map space.
- Home remains the quiet dashboard. The persistent 48 px bottom dock is
  `Home`, `Channels`, `Contacts`, `Map`, and `Settings`.
- Channels is list-first: Public, #bot, and #test are ordinary channel rows with
  selection and unread state, while Direct opens direct conversations. Opening
  a channel shows message bubbles and a composer affordance routed through the
  app model. Raw route and radio evidence is available from message detail, not
  printed across the conversation list.
- Message Detail and DM Thread are full-screen nested pages with one Back
  control, a scrolling body, and a sticky Reply action. Long Public text sizes
  before Technical details; opening a DM thread marks it read.
- Contacts mirrors the mobile client hierarchy: Find and Clear actions, Saved
  and Nearby sections, compact role-aware rows, and a direct Message action
  only for compatible contacts. Duplicate nearby entries are suppressed.
- Map is the actual current-view surface, not a setup dashboard. The map canvas fills the complete content region above the dock; there is no global diagnostic header or redundant map-local title row. OpenStreetMap Standard is dark-styled locally after decode, preserving the single built-in source/cache and attribution while giving bright signed-advert node markers and their names below them strong contrast. Marker refresh is a bounded lightweight overlay and does not rebuild tiles: it reads at most the 32 newest located nodes, displays at most eight, keeps its non-clickable marker/name layer aligned with the tile image during drag, and skips a marker when its required below-marker name would collide with an earlier marker, a control, progress/status copy, or attribution. A viewport-level 44 px hit area opens existing Contact Detail by retained fingerprint; the detail row says `Advert location`, and closing it reacquires the unchanged retained Map view. Its sparse edge overlays provide one-finger pan, direct 48x48-or-larger `-`, `+`, and `Center` controls, one `Options` setup action, an unobtrusive zoom/status badge, and the always-visible ASCII attribution `(c) OpenStreetMap contributors`. It starts at regional zoom 10, clamps user zoom to the runtime range 8 through 18 subject to provider limits, and `Center` returns to the saved manual location. D1L has no onboard GPS; peer coordinates are labelled `Advert location`, never live GPS. If the SD file gate is still preparing, the uncluttered overlay says `Waiting for SD` and the same physically opened generation resumes automatically when ready. While a bounded plan is active, the drag hint becomes a compact `Loading n/N` or `Downloading n/N` label above a thin completed-tile progress bar rather than leaving a static spinner/message with no progress feedback. The open Map requests only its visible 3x3 tile window; separately authorized background prefetch may run only while Map is closed and its saved provider, location, Wi-Fi, and SD prerequisites are ready.
- Contact Detail uses the full-height nested-page pattern. A canonical chat contact exposes only `Message` and `Contact options`; repeater, room, sensor, or unknown roles do not expose a dead Message action. Contact Options owns Route, Export, Rename, favorite/mute state, and the destructive contact-removal path. Export QR is actionable only for a retained full public key plus a known canonical MeshCore role; an unknown or malformed role shows a non-clickable unavailable row. Route, Export, and Rename return to Contact Options rather than flattening those functions back into Contact Detail.
- Removing a contact requires a dedicated confirmation page. `Cancel` and every `Back` path are non-destructive and restore Contact Options; only the explicit confirmation callback may delete the retained contact.
- Packet log is a Settings diagnostic screen with bounded recent packet rows,
  route rows, first route detail, and first packet detail.
- Mesh Roles follows `Settings -> Diagnostics -> Packet log -> Mesh Roles ->
  Rooms or Repeaters`: the full-height root contains only the two role
  categories, each child owns one bounded vertical list, and read-only
  observation rows have no RF or destructive callback.
- Storage follows `Settings -> Storage & maps -> SD Card -> Card status or Data
  locations`: all pages are full-height and read-only, Card status uses
  plain-language media guidance, Data locations owns the bounded list of store
  backends, and a fixed footer states that DeskOS never formats cards.
- Map follows `Map -> Map options -> Set location or Cache status` (canonical surfaces `map -> map_options -> map_location or map_cache`). OpenStreetMap Standard is built in; there is no provider, key, source, or account editor. Back from Set location or Cache status returns to Map options; saving a location returns to Map so loading can begin.
- Settings is a flat, always-visible grouped list rather than an accordion.
  Groups expose Packet log, Diagnostics, Terminal, Wi-Fi, Observer, SD Card,
  Map options, Radio, Display, Identity, About, and other supported device
  functions without hiding the core destinations.
- Radio is a full-screen Canada-preset-first page. Advanced controls stage
  frequency, bandwidth, SF, CR, TX power, and RX boost; Restore Canada and Save
  are explicit 48 px actions with live apply status.
- Modal advert sheet for zero-hop/flood actions.
- Toast feedback for touch actions.
- Lock/standby overlay with tap-to-unlock behavior.

The shell consumes `d1l_app_snapshot_t` from `app/app_model` and does not call MeshCore or HAL directly.

## Developer Diagnostics

Display color bars and touch-coordinate capture are developer-profile
USB-console commands. They are absent from production Home, dock, Settings,
command dispatch, and the shipping firmware payload.

## Navigation Rules

- Show one primary action per page. Put destructive, raw, flood, and advanced
  actions under a clearly named secondary menu with confirmation where
  appropriate. Qualification and developer test actions are absent from the
  production touch hierarchy.
- Prefer full-width menu rows with a disclosure cue over grids of equal-weight buttons when an item leads to a deeper function.
- A row tap opens its detail or action page; inline buttons are reserved for the single most common safe action.
- Modal and nested pages hide the dock, provide one clear Back or Close action, and cannot leave background navigation looking active.
- User-facing summaries use plain language. Protocol identifiers, fingerprints, raw packet bytes, and hardware diagnostics live under Technical details or Advanced.
- Contact actions follow `Contacts -> Contact Detail -> Contact options ->
  sub-function`. Destructive removal is never colocated with Message and never
  occurs from a Back, Cancel, keyboard-cancel, or child-page close callback.
- Mesh observation browsing follows `Settings -> Diagnostics -> Packet log ->
  Mesh Roles -> role list`. Do not flatten Rooms and Repeaters into one mixed
  scrolling sheet or attach actions to observation rows.
- Storage browsing never exposes raw setup-action slugs or mount/remount/delete/format callbacks. Keep Card status and Data locations separate, keep the no-format footer fixed while Data locations scrolls, and put serial-only maintenance commands outside the touch hierarchy.
- Interactive Map network policy is fail-closed: only while the actual Map is visible may firmware request at most the visible current-view 3x3 at one zoom per visible generation, selected by the user. A drag previews locally and commits one new bounded plan only on release; a `-`, `+`, or `Center` tap may likewise commit only the resulting visible view. Hiding Map cancels unfinished work. A completed exact-view Home-to-Map revisit must display the retained rendered frame without a new generation, network request, or SD tile reread. Tile cache remains the reboot/later-session reuse layer. There is no background fetch, multi-zoom prefetch, off-screen batch, or area download.
- Map page probes are network-suppressed navigation. `map`, `map_options`, `map_location`, and `map_cache` may open their pages for evidence, but probes never request map tiles and never mutate Wi-Fi, RF, or storage.
