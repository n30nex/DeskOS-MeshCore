# MeshCore DeskOS D1L 1.7.11

This patch makes Bluetooth phone pairing stable and visible on the SenseCAP
Indicator D1L.

## Fixed

- Wi-Fi and Bluetooth now run as explicit, mutually exclusive modes. Selecting
  one stops and releases the other stack before continuing.
- Tapping the Wi-Fi or BLE status icon on Home switches directly to that mode.
- Legacy upgrades that saved both switches as enabled migrate safely to BLE
  mode instead of starting both stacks.
- An incoming BLE connection wakes the display and automatically opens a large
  `123456` pairing prompt.
- The Bluetooth panel refreshes throughout pairing and clearly reports that
  Wi-Fi, online Map downloads, and Observer/MQTT are paused in BLE mode.

## Preserved

- Device identity, contacts, channels, messages, radio settings, Wi-Fi profiles,
  Observer settings, SD data, and the RP2040 bridge are preserved by the normal
  ESP32 application update.
- RF messaging and cached maps continue to work in either connectivity mode.
- All DeskOS 1.7.10 performance, Map, Observer, channel, timestamp, and flood
  advert fixes remain included.

## Install

Existing DeskOS devices use the application update image at offset `0x20000`.
Use the full-clean image only for a new installation or deliberate reset. The
RP2040 UF2 and SD card do not need to be changed for this patch.
