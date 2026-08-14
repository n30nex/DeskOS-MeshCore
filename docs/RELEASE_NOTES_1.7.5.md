# DeskOS MeshCore 1.7.5

DeskOS 1.7.5 makes remote repeater management, display wake-up, and cached maps
more dependable on the SenseCAP Indicator D1L.

## Highlights

- Server login always uses flood delivery, allowing the request to travel
  through the mesh when the repeater is beyond direct range.
- Closing a signed-in result page returns to the repeater manager, ready for the
  next command.
- Neighbours uses saved contact names where possible and shows readable age and
  SNR details.
- The display turns off and locks after ten idle minutes. The lock page fully
  covers every underlying control.
- One top-button press wakes the display. A deliberate double press while awake
  sends one normal advert.
- Cached SD map tiles no longer wait on the network-download pacing delay.

## Safety

- Flood delivery is limited to the login handshake. Signed-in commands retain
  the existing learned-route behavior.
- A held button cannot send repeated adverts, and automated validation performs
  no RF transmission.
- Passwords remain device-local, redacted, and removable.
- The update is flashed without erasing the D1L or formatting its SD card.

## Validation

- Native protocol parsing and button-gesture tests.
- Navigation, lock-layer, display-timeout, neighbour-name, and map-cache
  contracts.
- Complete host suite, exact GitHub Actions artifact, and physical D1L checks
  are recorded with the published release.
