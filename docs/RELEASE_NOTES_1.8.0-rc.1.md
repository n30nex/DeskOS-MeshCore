# DeskOS D1L 1.8.0-rc.1

This release candidate follows an audit of the full DeskOS firmware and its
installation path. It uses the production `full_feature` profile with
`conditional` SD history and the existing production update signer.

## Radio and phone companion

- Valid slow LoRa profiles can exceed the previous five-second transmit
  limit. Hardware and owner watchdog deadlines now cover the maximum frame
  airtime of the selected profile. Fast profiles retain their existing limit.
- Phone delivery deadlines include the applicable transmit budget and the
  permitted direct-to-flood retry.
- The Seeed driver's whole-dB SNR is converted to MeshCore's quarter-dB units,
  correcting local signal readings that previously appeared four times too low.
- Phone statistics report received frames, separate direct/flood counters,
  receive errors, signal measurements, and calculated transmit/receive airtime.
- Core statistics consistently describe the D1L's externally powered state.
- Wi-Fi signal strength refreshes from the associated access point without a
  scan or Wi-Fi driver calls from UI/status rendering.

## Touch interface and startup

- The early loading bar advances as retained stores are restored.
- Map labels have separate, bounded name and detail lines. Long UTF-8 names
  remain valid, labels stay clear of the current viewport controls, and labels
  themselves can be tapped to open node details.
- Map provenance and center-source labels fit their allocated space.
- Map attribution wraps at a readable size and remains inside the viewport.
- Home describes offline map mode instead of incorrectly requiring Wi-Fi.
  The map still checks actual saved tiles for the selected area.
- Unused legacy UI callbacks and an unused update logging declaration were
  removed.
- Signed-update boot acceptance requires the retained identity to be usable.
- A successful retained-store commit clears that store's recovered SD warning
  while preserving its historical error counters. Reads, unrelated-store writes,
  and a commit from a changed card cannot falsely clear the warning.

## Installation and support

- The browser console recognizes DeskOS JSON replies and matches each reply
  to its command, ignoring unrelated startup output.
- Startup verification waits up to two minutes over one connection while SD
  history loads, and verifies the selected build, display, identity and radio.
- USB disconnects, delayed writes and timeouts release pending console work.
  Secret-setting echoes and rejected replies remain hidden from the log.
- SD verification reads the actual nested device status and rejects stale or
  incomplete results. A preserving update can finish without optional SD
  storage; a fresh installation still requires all three setup stages.
- Selecting another device/build or starting another flash invalidates prior
  verification, and the D1L requires its expected USB identifiers.
- Hardware, build and install choices stay locked during flashing. Clean-install
  instructions distinguish replaced ESP32 data from SD files that remain on the card.

## Verification

The candidate passed the complete host test suite locally on the Pi 5:
**2,606 passed, two skipped**. One native credential-test compiler configuration
was corrected. Focused native checks exercise the actual radio
timing code, SNR boundary, companion statistics and delivery frames, boot
rendering, and map interaction. The corrected flasher console has also verified
the attached D1L, radio and prepared SD storage using actual USB replies.

The [tagged release record](https://github.com/n30nex/DeskOS-MeshCore/releases/tag/v1.8.0-rc.1)
records the exact image hashes and physical candidate acceptance. The official
Android app 1.49.0 interoperability run used 1.7.12; it is a baseline result,
not a claim of a new phone run on this candidate.

Physical candidate checks cover the preserving USB installer, retained identity
and SD storage, acknowledged DM to a local node, authenticated repeater status,
Wi-Fi signal refresh and return to BLE mode without a reboot, and all five main
screens. The current map view rendered all nine saved tiles with no network
requests. A new phone-app run, RP2040 reflash and physical signed-SD rollback
cycle are not claimed. The Pi's onboard Bluetooth central did not establish
its link, so it did not provide a replacement phone-interoperability result.

## Install

Use the complete release ZIP and its `START_HERE.md`, or select the candidate
on [flasher.canadaverse.org](https://flasher.canadaverse.org/). Use the preserving
update for existing DeskOS. The full 8 MB image is an explicit clean install
that replaces ESP32 identity and settings. The package includes the complete
RP2040 bridge UF2 and a matching signed SD update bundle.

Wi-Fi and Bluetooth remain exclusive modes. The D1L has no onboard GPS or
battery sensor. The current interface remains English-only, with the documented
DeskOS companion-command and local-confirmation boundaries.
