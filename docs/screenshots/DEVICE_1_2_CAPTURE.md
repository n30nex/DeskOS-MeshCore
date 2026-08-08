# DeskOS 1.2 actual-device screenshots

These 480x480 PNGs were captured from the production framebuffer of the
attached SenseCAP Indicator D1L on 2026-08-08.

- firmware version: `1.2.0`
- firmware build: `e15aff9eed9feb94bef6a81f90d62ac0f9fd9610`
- GitHub Actions run: [`31266143364`](https://github.com/n30nex/DeskOS-MeshCore/actions/runs/31266143364)
- source branch parent: `1df49a647ea84606c86baf2a4c8d3d8f6632c2a5`
- profile: `core_1_0`, conditional SD history
- capture mode: production read-only RGB565 framebuffer export

| Image | Active tab | Frame | Framebuffer CRC32 |
|---|---|---:|---|
| `device-1.2-home.png` | Home | 158 | `8C9356D4` |
| `device-1.2-channels.png` | Channels | 258 | `FA26F75E` |
| `device-1.2-contacts.png` | Contacts | 263 | `D06380A5` |
| `device-1.2-map-local-tiles.png` | Map | 1357 | `427862FA` |
| `device-1.2-settings.png` | Settings | 1430 | `697F8774` |

Each host-computed CRC matched the CRC reported by the firmware. The Map frame
was captured only after the production status reported 9 planned, 9 attempted,
9 cache hits, 9 rendered, 0 failed, and phase `ready` for the local SD tiles.

The capture path reported `public_rf_tx: false` and `formats_sd: false`.
Locations and public node labels are intentionally visible. Private messages,
passwords, private keys, and admin credentials are not shown.
