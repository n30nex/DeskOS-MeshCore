# DeskOS MeshCore 1.7.10

DeskOS 1.7.10 makes radio activity reach the screen faster and repairs the
remaining Bluetooth, Map, Observer, channel, timestamp, and flood-advert beta
issues on the SenseCAP Indicator D1L.

## What changed

- **Reliable top button:** a deliberate double press queues one flood advert
  immediately, keeps the interface responsive, and reports the final result.
- **Stable BLE pairing:** repeat pairing replaces only the connecting peer's
  stale bond. Reduced Wi-Fi receive-buffer pressure leaves more internal heap
  for encrypted companion requests while Wi-Fi remains enabled.
- **Faster Contacts:** node queries snapshot contacts once, sort small indexes
  instead of large records, and return only the bounded screen result set.
- **Retained Map view:** Options and ordinary navigation no longer discard the
  current frame. A healthy partial tile pass resumes from SD.
- **Editable Observer region:** set a three-letter IATA code such as `YYC` on
  the Observer screen. The two default secure MeshCore Canada brokers and the
  optional custom broker remain unchanged.
- **Hashtag channels:** add `#chat`, `#yyc`, `#yyc-weather`, or another exact
  hashtag to derive the standard interoperable MeshCore channel secret.
- **Useful receive times:** an implausible or absent wire timestamp falls back
  to trusted local arrival time for newly received channel messages.

## Safety

- DeskOS remains a non-forwarding MeshCore client.
- No automatic public RF traffic was added. The flood advert requires the
  physical double press and retains its one-minute cooldown.
- Observer remains opt-in and never publishes private keys or saved contacts.
- Existing-device app updates preserve identity, channels, contacts, settings,
  message history, SD contents, radio settings, and RP2040 firmware.

## Recommended update

Existing DeskOS users should install the preserving app update at `0x20000`.
Use the full 8 MB image only for a fresh or intentionally erased D1L.
