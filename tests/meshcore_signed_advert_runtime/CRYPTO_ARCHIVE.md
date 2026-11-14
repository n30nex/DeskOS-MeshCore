# Pinned host-test dependency

`Crypto-0.4.0.tar.gz` is the unchanged rweather/Crypto 0.4.0 source archive
from the PlatformIO registry (version ID 43204), obtained with PlatformIO
Core. It is used only by the signed-advert host test, not linked into the
DeskOS firmware. Its sources and licenses remain inside the archive.

- Upstream: https://github.com/rweather/arduinolibs
- Release commit: `61e84b220fc8dfe83c2e04716d0fa88cfaddadf6`
- Registry archive: https://dl.registry.platformio.org/download/rweather/library/Crypto/0.4.0/Crypto-0.4.0.tar.gz
- Size: 162696 bytes
- SHA256: `1867740aad0d61bdcbac25f6dbc8eefe6eed9e7b37f48d9d0b9d80500ad431e0`

The test verifies the archive and each selected source against the existing
manifest. Keeping the exact archive here makes that check independent of
registry API changes and outages. No cryptographic implementation changed.
