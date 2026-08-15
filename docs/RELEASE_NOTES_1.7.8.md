# DeskOS MeshCore 1.7.8

DeskOS 1.7.8 repairs connectivity and screen updates on the SenseCAP Indicator
D1L.

## What changed

- **MQTT Observer works:** received MeshCore packets and device health go to
  both `mqtt1.meshcore.ca` and `mqtt2.meshcore.ca` over secure WebSockets.
- **Standard identity:** each broker gets its own short-lived Ed25519-signed
  login token and the normal `meshcore/YKF/<public-key>/...` topics.
- **Custom broker:** one additional WSS or MQTT-TLS broker can be configured.
- **Fast handoff:** radio reception only copies a bounded packet record;
  formatting and MQTT delivery happen on the Observer task.
- **Wi-Fi plus Bluetooth:** companion mode no longer turns Wi-Fi off, and the
  pairing PIN is `123456`.
- **Fresh Recent list:** saved contacts remain, but boot-relative heard times
  no longer make an old advert look new after restart.
- **Message time:** newly received and sent channel messages show a real local
  clock time when trusted time is available.
- **Progressive Map:** saved tiles remain visible while missing tiles retry.

## Safety

- Observer remains opt-in and uses bounded queues.
- The D1L remains a non-forwarding MeshCore client; this update does not add
  automatic RF traffic.
- Private keys, passwords, contacts, and plaintext message history are not
  included in Observer status or diagnostics.
- Existing identity, channels, contacts, settings, history, radio settings,
  SD contents, and RP2040 firmware are preserved by the app-only update.

## Deferred

The optional Indicator temperature, humidity, and CO2 sensors are planned for
a later feature release after their exact hardware variants can be tested.
