# DeskOS MeshCore 1.7.9

DeskOS 1.7.9 repairs Wi-Fi, Bluetooth companion mode, and MQTT Observer
coexistence on the SenseCAP Indicator D1L.

## What changed

- **Wi-Fi plus Bluetooth:** the Bluetooth stack and Observer worker now place
  their larger allocations in PSRAM, preserving scarce internal memory for
  Wi-Fi and the radio.
- **Bluetooth companion mode:** enabling Bluetooth no longer starves or drops
  an active Wi-Fi connection.
- **Reliable Observer startup:** secure MQTT waits for network-validated time
  before creating signed broker credentials or opening TLS connections.
- **Bounded retries:** Observer startup and reconnect work yield between
  attempts instead of repeatedly consuming the UI and networking loop.
- **Lower connection pressure:** unused Bluetooth services and oversized
  controller pools are removed, retained route/Observer buffers live in
  PSRAM, and the two secure broker handshakes start sequentially.
- **Useful broker diagnostics:** Observer status now distinguishes broker
  rejection, TLS failure, and socket failure without exposing credentials.
- **Two default brokers:** Observer continues to publish to both
  `mqtt1.meshcore.ca` and `mqtt2.meshcore.ca`, with the custom broker option
  unchanged.
- **Existing 1.7.8 fixes retained:** recent adverts, message timestamps,
  progressive maps, and the top-button flood advert remain included.

## Safety

- Observer remains opt-in and the D1L remains a non-forwarding MeshCore
  client.
- This release does not add automatic public RF traffic.
- Broker credentials, private keys, passwords, contacts, and message contents
  stay out of status and diagnostics.
- The app-only update preserves identity, channels, contacts, settings,
  message history, radio settings, SD contents, and RP2040 firmware.

## Recommended update

Existing DeskOS users should install the preserving update image at `0x20000`.
Use the full 8 MB image only for a fresh or intentionally erased device.
