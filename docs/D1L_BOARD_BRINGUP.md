# D1L Board Bring-Up

> **HISTORICAL BOARD-BRING-UP PROCEDURE — NOT CURRENT RELEASE ROUTING.**
> Preserve the hardware observations below, but do not use its COM assignments
> or broad qualification sequence for RC1. Use the
> [project README](../README.md), [RC1 user guide](USER_GUIDE_D1L.md),
> [RC1 test plan](TEST_PLAN_D1L.md), and
> [SD-card guide](D1L_SD_CARD_GUIDED_INSTALL.md).

## Phase 1 Order

1. Boot serial log and `version`.
2. `board` confirms D1L target and board init status.
3. `i2c` scans for the IO expander and touch controller.
4. `display test` paints color bars.
5. `touch test` reports coordinates.
6. `button` reports the user button state from GPIO38.
7. `backlight <0-100>` verifies PWM dimming on GPIO45.
8. `rp2040 status` reports the ESP32-S3 UART bridge on TX GPIO19 / RX GPIO20.
9. `radiohw` reads SX1262 status via SPI and expander CS without enabling TCXO.
10. `radio get` confirms Canada/USA defaults.
11. `packets` confirms the Phase 1 packet ring is ready for later LoRa/MeshCore events.
12. `health` captures heap, PSRAM, uptime, and reset reason.

## Known D1L Pins

See [boards/seeed_indicator_d1l/pinmap.json](../boards/seeed_indicator_d1l/pinmap.json).

Important defaults:

- I2C: SDA GPIO39, SCL GPIO40.
- Display RGB: HSYNC GPIO16, VSYNC GPIO17, DE GPIO18, PCLK GPIO21, data GPIO15..0 as Seeed SDK maps them.
- Backlight: GPIO45.
- Button: GPIO38 active low.
- SX1262 SPI: SCLK GPIO41, MOSI GPIO48, MISO GPIO47.
- SX1262 expander pins: CS 0, reset 1, busy 2, DIO1 3.
- TCXO default: `NONE`.
- RP2040 bridge UART: ESP TX GPIO19, ESP RX GPIO20, reset expander pin 8.

## Phase 1 Risks

- The D1L uses a 16-bit TCA/PCA9535-style expander. Do not replace it with an 8-bit PCA9554-only abstraction.
- The Seeed SDK can detect a radio `VER` expander pin and later enable DIO3 TCXO control. MeshCore DeskOS D1L keeps TCXO disabled by default unless a board revision is proven to require it.
- Display failures can be caused by panel variant, ST7701S init table, RGB color order, PCLK polarity, or PSRAM framebuffer mode. Start with Seeed's D1L/GX defaults.
- A responsive serial console or healthy board status is not real mesh validation. Phase 2 RF validation must show fresh RX/TX counter movement from a second known-good MeshCore node.

## Hardware Validation Status

Current hardware validation status is tracked in [ROADMAP.md](ROADMAP.md),
[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md), and
[KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md). Current D1L release validation uses
COM12 for the ESP32 app/console and production RP2040 bridge control. COM16 is
used only while bounded RP2040 USB smoke or UF2 maintenance firmware is active;
the production bridge intentionally exposes no USB CDC port.

Scripts still intentionally stop before flashing unless `D1L_PORT` or `--port` is supplied.
