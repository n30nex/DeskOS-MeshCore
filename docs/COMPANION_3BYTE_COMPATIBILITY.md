# BLE and 3-Byte Companion Compatibility

Updated: 2026-08-13 for DeskOS 1.5

MeshCore DeskOS D1L must be compatible with MeshCore companion clients in both meanings used by current MeshCore references.

## 1. Companion Transport Header

MeshCore serial and Wi-Fi companion links delimit each companion protocol payload with a 3-byte transport header:

```text
[type][length_lsb][length_msb][payload...]
```

- App/client to radio: type byte `<` (`0x3c`).
- Radio to app/client: type byte `>` (`0x3e`).
- Length is an unsigned 16-bit little-endian payload length.
- Payload is the MeshCore companion protocol frame.

Reference evidence:

- `third_party/MeshCore/src/helpers/ArduinoSerialInterface.cpp` writes `>` plus length LSB/MSB and reads `<` plus length LSB/MSB.
- `third_party/MeshCore/src/helpers/esp32/SerialWifiInterface.cpp` uses the same framing for Wi-Fi and documents the 3-byte frame header.

DeskOS 1.5 status:

- `main/comms/companion_3byte.*` implements the ESP-IDF C codec.
- `tools/d1l/companion3.py` mirrors the codec for host tests and future tooling.
- `companion status` reports the active compatibility contract through the USB JSONL console.
- `main/comms/ble_companion.*` exposes the official MeshCore BLE service, RX,
  and TX UUIDs and adapts characteristic values to the same bounded internal
  three-byte frames.
- `main/comms/ble_companion_protocol.*` connects those queues to the normal
  single-owner MeshCore stores and commands.

## BLE security boundary

- Pairing requires Secure Connections, MITM protection, a displayed six-digit
  passkey, encryption, authentication, and bonding.
- A connection is not protocol-ready until the client subscribes to TX
  notifications.
- Repeated pairing replaces that connecting peer's stale bond and retries.
- Private-key import/export, factory reset, and remote reboot are disabled over
  BLE even though those command numbers exist in the wider companion protocol.
- Frames are bounded by the negotiated ATT MTU and fixed queues. Oversize,
  malformed, unauthenticated, or unsubscribed traffic fails closed and is
  counted without logging payloads or secrets.
- Wi-Fi and BLE are exclusive modes. Their transitions are serialized by the
  connectivity manager, which fully stops one stack before starting the other.

## 2. 3-Byte Path Hash Support

MeshCore packet path metadata can encode 1-, 2-, or 3-byte path hashes. The path length byte stores hop count in bits 0-5 and hash-size code in bits 6-7:

- `0b00`: 1-byte path hashes.
- `0b01`: 2-byte path hashes.
- `0b10`: 3-byte path hashes.
- `0b11`: reserved / unsupported.

Reference evidence:

- `third_party/MeshCore/docs/packet_format.md` documents 3-byte path hashes as supported in current firmware.
- `third_party/MeshCore/docs/faq.md` explains that firmware 1.14+ repeaters forward 1-, 2-, and 3-byte path-hash packets, while older repeaters drop 2- and 3-byte path-hash packets.

Project policy:

- D1L companion metadata and packet logs must preserve the encoded hash size and raw path bytes.
- Default message path hash size remains 1 byte for maximum legacy repeater compatibility until the user or client selects 2 or 3 bytes.
- The UI/settings model preserves the selected path-hash mode without implying
  that it changes repeater forwarding behavior.
- Diagnostics should warn that 3-byte paths can be dropped by repeaters older than MeshCore firmware 1.14.
