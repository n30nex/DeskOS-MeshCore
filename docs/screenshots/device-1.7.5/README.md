# Physical DeskOS 1.7.5 captures

These four 480x480 PNG files were captured from the RGB565 shadow framebuffer
of the physical SenseCAP Indicator D1L after the production `v1.7.5` artifact
was flashed and checked on 2026-08-14.

The device reported:

- firmware: `MeshCore DeskOS D1L 1.7.5`
- build commit: `b719faaed93032211988c00b6a5c0c0ee74ef60b`
- release profile: `full_feature`
- ESP-IDF: `v5.5.4`
- storage history mode: `conditional`

| Capture | Active tab | Frame CRC32 | Device CRC matched |
|---|---|---:|---|
| `home.png` | Home | `577AE337` | Yes |
| `messages.png` | Channels | `E29E3F5B` | Yes |
| `nodes.png` | Contacts | `E34E6646` | Yes |
| `settings.png` | Settings | `C686BD48` | Yes |

The capture path was read-only. Every receipt reported `public_rf_tx: false`
and `formats_sd: false`. The device was returned to Home after collection.
Private messages, passwords, keys, long identifiers, and precise location data
are not present in the published frames.

