# DeskOS D1L 1.8.0-rc.5

This candidate repairs a radio stall observed while using the official
MeshCore phone app. RC4 could accept one channel message into its queue and
return success while the radio task was stuck; later sends returned an error.
The wider phone check also exposed missing discovery and ordinary-contact
telemetry handlers and a contact-edit persistence race.

## Radio and companion repair

- The SX1262 BUSY wait has a one-second deadline and always yields at least
  one RTOS tick. A stuck signal, failed expander read or failed SPI transfer latches a fault instead
  of holding the radio task indefinitely. Later SPI operations fail closed.
- Recovery belongs to the radio owner and excludes the driver IRQ path while
  resetting the peripheral. It reuses existing timers and restores the saved
  RF profile before reporting radio readiness. Failed recovery stays visible
  and retries with a bounded delay. An uncertain TX is terminalized once and
  is not automatically retransmitted after the reset.
- Companion channel sends use the existing cancellable command path and wait
  for the radio owner to start the operation. A rejected or expired request
  produces an error, and an expired queued copy cannot later transmit. The
  physical touchscreen composer keeps its asynchronous API.
- Hardware diagnostics use the driver's serialized SPI path; a support probe
  cannot change chip select in the middle of a radio transaction. Mesh status
  reports the current fault and recovery counts. Channel-command diagnostics
  also retain text type and byte length, without message contents.
- Nearby-node discovery now connects the phone's request to the existing
  zero-hop radio operation. Filters, request identifiers, optional age filters
  and both full-key and key-prefix replies retain their protocol meanings.
- Telemetry requests to ordinary chat clients and sensors use the existing
  correlated radio request and the remote node's permissions. Both current
  binary and older telemetry replies reach the phone. Repeater and room queries
  retain the existing authenticated management session.
- Explicit contact edits acquire foreground storage ownership before locking
  the contact store. A concurrent radio or storage operation cannot cancel
  that edit halfway through a healthy SD write. Waiting is bounded, and media
  generation checks and rollback on genuine persistence failure remain active.
- The USB test helper recognizes the actual contact-flag and location replies,
  avoiding a false timeout after an edit that already succeeded.
- Radio settings reject the unsupported repeater-mode flag before applying
  changes, instead of acknowledging an option the D1L cannot enable.

Drafts, quoted replies, clipboard, identity, channel configuration, signed
updates and SD data retain the RC4 behavior. No path formats the card. The
RP2040 firmware is unchanged.

## Validation

Native checks exercise transient/permanent BUSY, I2C read failure, no SPI after
a latched fault, exclusive recovery, timer reuse, profile restoration and
companion error mapping. Contact checks cover ownership contention and unchanged
state on failure; phone-query tests execute the production response encoders,
including request correlation, bounded payloads and stale-result rejection.
Exact local Pi suite/build and physical device/app
results belong in the tagged release record. A native injected fault is not a
claim that a physical hardware fault was injected on the production D1L.

Build and test locally on the Pi with pinned ESP-IDF 5.5.4. Use the preserving
installer for an existing DeskOS device; the full 8 MB image replaces its
ESP32 identity and settings. Signed SD files go in `deskos/updates` on a
prepared FAT32 card. This remains a release candidate; the parity record and
release notes retain the remaining feature and physical-acceptance limits.
