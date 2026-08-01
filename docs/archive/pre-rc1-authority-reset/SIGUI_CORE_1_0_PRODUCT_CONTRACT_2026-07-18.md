HISTORICAL RECORD — DO NOT EXECUTE

This document predates the RC1 authority reset. It is retained only for
provenance. It cannot create work, tests, evidence requirements, or release
gates. See `docs/RC1_SCOPE.md` and `docs/ROADMAP.md`.

# MeshCore DeskOS D1L Core 1.0 Product Contract

**Contract ID:** `core_1_0`
**Target tag:** `v1.0.0`
**Target device:** Seeed SenseCAP Indicator D1L
**Radio region default:** USA/Canada
**SD mode:** `conditional`
**Status:** current DeskOS 1.0 / RC1 production contract
**Supersedes:** the minimal 24-hour Core scope retained later in this file

---

## Current DeskOS 1.0 / RC1 contract (2026-07-25)

DeskOS D1L is a touch-first, non-forwarding MeshCore client. The production
build is immutable `core_1_0` with `conditional` SD support. A normal user can
operate the supported surface from the touchscreen; USB remains an explicit
diagnostic and URI-import path.

### Supported RC1 surface

| Capability | RC1 contract |
|---|---|
| Identity/adverts | Retained exact identity; signed flood advert is queued after MeshCore RX starts |
| Messaging | Public and custom channels; send/receive/history/search/unread; DM exact-key routing, ACK, retry and terminal state |
| Contacts | USB MeshCore URI import; touchscreen rename, favorite, mute and confirmed remove |
| Nodes | Signed roles and evidence; Finder; up to 512 retained nodes; only unlocated entries may be replaced at capacity, while signed-location markers remain until explicit clear |
| Path tools | Repeater Ping as zero-hop TRACE; verified-contact PATH/TRACE with pending/timeout/reply/RTT/RSSI/hop-SNR truth |
| Map | Configured device center; valid signed advert coordinates; zoom 8–18; attributed built-in OSM visible-view cache |
| Background Map | Connected Wi-Fi, ready SD and an authorized HTTPS provider permitting offline storage and background prefetch; nodes bounded to 200 km |
| Wi-Fi | Scan, save multiple profiles, select, delete, connect, disconnect and reconnect |
| Settings | Region/preset, radio parameters and power, RX boost, display and fixed-offset time |
| Administration | Verified repeater/room login/logout, route state, status, telemetry, neighbours, ACL, bounded CLI, confirmed settings mutations and current-session room posts |
| Storage | SD-primary history, contacts/nodes, routes, packets, Map and exports; visible live-only RF degraded mode without history fallback |
| Diagnostics | Bounded packets/raw preview, event terminal, storage/Map/Wi-Fi/crash status |
| Observer | Optional TLS-only `mqtts://`, QoS 1, bounded health/location payload without message text, keys or contacts |

Finder results remain unverified until a signed advert arrives. Public display
names never alias-match into a DM target. Touchscreen contact URI import is not
an RC1 claim; use `contacts import <meshcore-uri>` over USB.

### Storage and Map authority

- Users prepare a 32GB-or-larger FAT32 card. Firmware and validation never
  format, repair or overwrite foreign media.
- SD is the primary retained-data authority. Missing/unusable SD displays
  restrictions and retains basic live Public/channel/DM RF operation; retained
  history, Map cache/download and exports are unavailable.
- Default NVS contains bounded device/configuration, identity, channel,
  Wi-Fi/Observer, display/time, boot/recovery and diagnostic metadata. It is
  not a silent retained-history fallback.
- Built-in OpenStreetMap Standard displays
  `(c) OpenStreetMap contributors` and may fetch only the visible current-view
  3×3 at one zoom per visible generation. Read-only Map probes request no
  tiles.
- Authorized-provider background prefetch pauses while interactive Map is
  open. It uses zoom 8–18 as space permits, caps Map allocation at 60%, and
  preserves at least 8 GiB outside that allocation.

### Security and intentionally deferred scope

- Remote mutations require the exact authenticated target/session and local
  confirmation. Sensitive input is redacted, not persistently retained or
  logged, and wiped from volatile confirmation buffers.
- DeskOS never forwards third-party MeshCore traffic. Release automation may
  transmit only the single tokenized final-gate Public message after the
  operator explicitly supplies `--authorize-public-tx`; every other automated
  path remains Public-silent.
- BLE companion transport, contact/channel QR sharing, and signed
  OTA/update/recovery product workflows are deferred to 1.5 / RC2 and must
  remain unavailable in RC1.

### Release artifact and physical gate

Release firmware must be built by the exact candidate commit's GitHub Actions
run using pinned ESP-IDF 5.5.4. Verify the downloaded package checksum tree,
inventory, provenance, SBOM, `core_1_0` profile and `conditional` SD mode.
Local firmware builds and predecessor packages cannot qualify.

The release-closing D1L is attached to Pi 5 host `neopi5` and may be selected
only by:

```text
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
VID:PID 1a86:7523
```

Never substitute a raw `/dev/ttyUSB*` path or stale Windows COM assignment,
and never enumerate or probe another Pi serial device.

Public release requires one bounded gate on the exact downloaded and flashed
Actions artifact:

1. boot and five-root DeskOS navigation;
2. boot advert and one operator-authorized Public message;
3. one DM with ACK;
4. contact PATH/TRACE and repeater Ping;
5. repeater login and authenticated query;
6. Wi-Fi reconnect;
7. SD write/remount plus missing/unusable-card degraded notice;
8. authorized-provider Map download and offline cache revisit;
9. automated 12-surface navigation on the exact artifact, retaining the
   operator's already-completed display/touch/keyboard/scroll acceptance
   without repeating a manual campaign.

No soak is required. Any unexpected reboot, artifact mismatch, destructive SD
operation, missing required workflow or unconfirmed security-sensitive
mutation is a no-go.

---

## Historical 24-hour minimal-Core contract

Everything below this heading records the superseded July 18 minimal-Core
proposal and predecessor evidence requirements. Its unavailable-feature
matrix, NVS-authoritative fallback, USB-recovery claim, COM routing and soak
requirements do not describe the current RC1 product or release gate.

### Historical hardware route (2026-07-24)

The release-closing D1L is currently attached to the Raspberry Pi 5 host
`neopi5`. Hardware work runs from the unprivileged, key-only development
account `siguidev` and must select the device only through:

```text
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
```

The expected USB identity is `1A86:7523`. The link currently resolves to
`/dev/ttyUSB2`, but that kernel-assigned name is observational only and must
not be used as the release target. `COM12` remains the valid Windows
alternative when the D1L is moved back to the Windows host. `COM8`, `COM11`,
and `COM29` remain forbidden. `COM16` remains reserved for separately
authorized SD/RP2040 work and is never the Core D1L app, console, or flash
target.

The move to `neopi5`, successful login, or discovery of the USB device does
not close a release gate. Exact-SHA Actions/package binding, a non-erasing
flash, UI and manual review, reboot and retained-state proof, protocol-time
migration, controlled RF/DM, active and idle soak, installation review, and
the final Core audit all remain fail-closed. Controlled-peer RF/soak work also
remains blocked until the `siguidev` account has narrowly scoped, verified
access to the required peer status and control resources.

---

### Historical 1. Product statement

MeshCore DeskOS D1L Core 1.0 is a touch-first, non-forwarding MeshCore desk client focused on reliable messaging and local mesh visibility.

A user must be able to:

- power on to a stable Home screen;
- see device, radio, message, and health truth;
- read and compose Public messages on the fixed default Public channel;
- discover a verified node/contact and exchange direct messages;
- see truthful DM delivery/failure state;
- inspect heard nodes and packet activity;
- configure the Canada/USA radio profile;
- retain settings and core message state across reboot;
- recover or reinstall through USB using the packaged instructions.

Core 1.0 must not imply that Map, Wi-Fi, BLE, OTA, administration, multi-channel management, Observer/MQTT, GPS/location, or advanced tools are supported.

---

### Historical 2. Supported capability matrix

| Capability | Core 1.0 state | Release condition |
|---|---|---|
| Board initialization | Supported | Exact-candidate smoke |
| 480×480 display | Supported | Manual display confirmation and pixel/UI probe |
| Touch and backlight | Supported | Exact-candidate touch/manual check |
| Home | Supported | Core navigation/UI acceptance |
| Public messages | Supported | Compose/read/send/receive on fixed default channel; no uncontrolled automated Public RF |
| Direct messages | Supported | Exact controlled-peer inbound/outbound, ACK/PATH, direct route, retained state |
| Basic contacts | Supported | Verified advert/heard-node to DM path; no ambiguous prefix |
| Nodes | Supported | Bounded list/detail; no unsupported actions |
| Packets | Supported | Read-only packet log/search/filter |
| Routes/signals | Read-only support | Internal DM route plus bounded diagnostics |
| Radio profile | Supported | USA/Canada defaults and explicit settings |
| Identity/adverts | Supported | Existing exact candidate conformance plus device smoke |
| Retained NVS | Supported | Reboot/non-erasing-upgrade persistence |
| FAT32 SD history | Conditional | All exact-candidate SD gates pass; otherwise disabled |
| Diagnostics/crashlog/health | Supported | Exact candidate telemetry and soak |
| USB install/recovery | Supported | Packaged, checksum-verified instructions |
| Fixed UTC offset/time truth | Supported | Truthful approximate/unavailable state; no false authority |

---

### Historical 3. Unavailable capability matrix

The following are unavailable in Core 1.0 and must not be reachable:

| Capability | Required Core behavior |
|---|---|
| Map | No Home/dock/settings entry; no tile worker started |
| Wi-Fi user control | Runtime remains off; mutating commands rejected |
| BLE | Build/package reports unavailable; PR #199 not merged |
| Multi-channel management | Fixed default Public channel only; create/import/export/select/remove hidden/rejected |
| Repeater/room administration | Hidden/rejected; no remote mutation |
| Observer/MQTT | Hidden/rejected; no background task |
| Signed SD update / OTA | Hidden/rejected; USB install/recovery only |
| GPS/location | Hidden/rejected; no location claim |
| Mutable terminal/log UI | Hidden/rejected |
| Advanced QR/emoji | Hidden/rejected |
| User-facing TRACE/PATH tool | Hidden unless a read-only diagnostic is explicitly qualified |
| Notification system | No production claim beyond existing unread counters |

---

### Historical 4. Release-profile authority

Add one immutable capability authority:

```text
main/app/release_profile.h
main/app/release_profile.c
```

The Core build must compile with:

```text
D1L_RELEASE_PROFILE=core_1_0
```

The exact implementation mechanism may use Kconfig, CMake definitions, or a generated header, but it must be:

- deterministic;
- included in package metadata;
- testable from source;
- visible in `version` and `health`;
- bound to the exact Actions artifact;
- impossible for a user setting to change at runtime.

---

### Historical 5. UI rules

Core navigation consists of:

1. Home
2. Messages
3. Nodes
4. Packets
5. Settings/Tools

Messages may contain Public and DM views.

Settings may contain:

- identity summary;
- radio profile;
- retained storage summary;
- display/backlight;
- timezone/fixed offset;
- diagnostics;
- about/version;
- recovery/help.

Unavailable features must be omitted. A read-only capability list may explain that they are planned for later releases.

No dead button may open a partial controller.

---

### Historical 6. Command rules

#### Permitted categories

- version, board, health, crashlog;
- display/touch/backlight;
- settings needed by Core;
- identity;
- radio;
- Public and DM message operations;
- contacts/nodes needed by Core;
- packet/route/signal read-only diagnostics;
- storage status and Core-supported retention;
- controlled reboot and documented recovery.

#### Rejected categories

Unavailable feature mutations must fail before side effects with:

```json
{
  "ok": false,
  "code": "ESP_ERR_NOT_SUPPORTED",
  "release_profile": "core_1_0",
  "feature": "<id>"
}
```

Read-only status may return `ok=true` with `available=false`, or the bounded unsupported response. It must never imply runtime support.

---

### Historical 7. Storage contract

#### Always supported

- settings;
- identity;
- Public and DM retained state within the documented bounded capacity;
- read-state markers;
- contacts needed by Core;
- route state needed by DM;
- crashlog;
- NVS fallback.

#### Conditional SD

SD is supported only when the exact final candidate proves:

- correct paired ESP32/RP2040 artifacts;
- FAT32 card present/mounted/root-ready;
- file operations and atomic rename;
- reboot/remount;
- physical removal and reinsertion;
- file canary;
- retained readback;
- stable 30-minute window;
- no format action.

When conditional qualification fails:

- `sd_history=false`;
- UI hides SD data actions;
- package omits RP2040 release payloads;
- NVS remains authoritative;
- release notes state SD is deferred.

---

### Historical 8. RF contract

- D1L is non-forwarding.
- Default region is USA/Canada.
- Uncontrolled or implicit automated default Public transmission is prohibited.
  The current final gate permits exactly one tokenized send only after explicit
  `--authorize-public-tx` operator consent.
- Controlled DM or a configured private `#test` channel is used for RF proof.
- Direct messages must have truthful queued/sent/acknowledged/retrying/failed state.
- No malformed, unauthenticated, duplicate, or replayed payload may create a visible duplicate or incorrect ACK.
- COM8, COM11, and COM29 are forbidden.
- The current D1L app/console path is the exact `neopi5` stable by-id link
  declared above; `COM12` remains the valid Windows alternative.
- A raw `/dev/ttyUSB*` name is never authoritative release identity.
- COM16 is never the Core D1L app/console/flash target.
- A controlled peer must use a distinct explicitly assigned allowed path.

---

### Historical 9. Minimum production evidence

Core 1.0 requires:

1. exact Actions workflow green;
2. exact downloaded artifacts and verified checksums;
3. package profile binding;
4. non-erasing exact-target flash receipt, bound to the stable device identity
   (`neopi5` by-id path for the current route, or `COM12` on the Windows
   alternative);
5. profile-aware core smoke;
6. display/touch manual confirmation;
7. supported UI corruption/navigation/scroll/compose evidence;
8. controlled RF/DM acceptance;
9. reboot/persistence evidence;
10. 60-minute active plus 30-minute idle soak;
11. crash/heap/stack/LVGL health;
12. installation and recovery docs;
13. `core_release_ready=true`;
14. zero known Core P0;
15. zero known Core crash/data-loss/security P1.

The Full Feature audit is expected to remain false.

---

### Historical 10. Release notes minimum

Release notes must include:

- “Core 1.0” in the title;
- exact supported and unavailable matrices;
- SD support state;
- firmware commit;
- Actions run;
- SHA-256 values;
- installation and recovery steps;
- no on-device SD formatting;
- current known limitations;
- support/reporting channel;
- explicit statement that Map, Wi-Fi, BLE, OTA, multi-channel management, administration, Observer/MQTT, and location are deferred.

---

### Historical 11. No-go conditions

Do not tag when:

- exact candidate identity is not proven;
- any core P0 remains;
- any crash/data-loss/security P1 remains;
- DM interoperability is missing;
- unsupported features remain reachable;
- checksums or provenance fail;
- device reboots unexpectedly;
- retained state is lost;
- soak fails;
- SD is advertised without exact SD evidence;
- evidence is stale, simulated, dry-run-only, or from a predecessor SHA;
- forbidden ports or SD formatting are used.
