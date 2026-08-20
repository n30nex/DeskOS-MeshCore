# MeshCore DeskOS D1L 1.7.12

This patch stabilizes Bluetooth phone synchronization and makes the remaining
channel, MQTT, and recent-contact controls obvious on the SenseCAP Indicator
D1L.

## Fixed

- BLE receive and transmit frame scratch space is allocated in PSRAM instead
  of on the 4 KB NimBLE host-task stack. Initial phone synchronization can pull
  the complete saved channel list without exhausting that callback stack.
- The BLE TX characteristic now matches the official MeshCore readable and
  notify shape, negotiates a 517-byte ATT MTU explicitly, and spaces companion
  replies by 60 ms so phone synchronization is not starved or flooded.
- The NimBLE host task has a measured 6 KB stack reserve for Android's initial
  secure pairing, MTU, service discovery, channel, and contact synchronization
  burst.
- `ble status` now reports the secure-link, MTU, queue, frame, and protocol
  counters needed to diagnose a phone session without enabling verbose logs.
- Channels always shows **Add** beside **Direct**. The existing create and
  import screen is reachable even when Public is already configured.
- Connections now names **MQTT / Observer** directly. Its panel labels the
  editable three-letter **IATA region** and confirms a successful save.
- A repeated verified advert refreshes boot-local last-heard time and Contacts
  sorting while leaving retained identity, location, sequence, and persistence
  state unchanged.

## Preserved

- Wi-Fi and Bluetooth remain explicit, mutually exclusive modes.
- Pairing still requires Secure Connections, MITM protection, encryption,
  authentication, bonding, and the displayed `123456` PIN.
- Device identity, contacts, channels, messages, radio settings, Wi-Fi
  profiles, Observer settings, SD data, and the RP2040 bridge are preserved by
  the normal ESP32 application update.
- DeskOS never formats the SD card and never sends RF traffic during an update.

## Install

Existing DeskOS devices use the application update image at offset `0x20000`.
Use the full-clean image only for a new installation or deliberate reset. The
RP2040 UF2 and SD card do not need to be changed for this patch.
