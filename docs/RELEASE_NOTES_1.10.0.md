# DeskOS D1L 1.10.0

The on-device keyboard now supports saved QWERTY, AZERTY and QWERTZ letter
layouts. Select **Settings / Display & clock / Keyboard** to cycle layouts.
QWERTY remains the default when upgrading. The selection updates existing
inputs, including hidden sheets, without replacing text, moving the cursor
or changing upper/lowercase mode.

Tap **áé** (or **ÁÉ**) for accented Latin characters. **ABC/abc** changes case
on that page and **Back** returns to the alphabet. **1#** opens numbers and
symbols; **2#** opens additional punctuation such as dollar signs, brackets
and backslash. All printable ASCII characters are reachable, including those
needed by some passwords. The glyphs are included at both text sizes.
This is Latin character entry, not interface translation, an input method for
all languages, or a complete national desktop keyboard. The UI stays English.

Message capacity is still 138 UTF-8 bytes. Accented letters can occupy more
than one byte; an over-limit draft shows the existing warning and cannot be
sent. Nothing is transmitted by changing the layout or opening a character
page. Pressing the existing checkmark retains each input's normal action.

The layout uses an independent display-preference key. Existing identity,
settings, history formats, BLE protocol, radio behavior, signed updater and
RP2040 firmware are unchanged. Use the preserving USB update for an existing
DeskOS installation. Firmware never formats the card.

Native checks execute the actual LVGL keyboard, event handling and glyph
rendering. They cover active/hidden keyboards, cursor and draft preservation,
case/page changes, multibyte backspace, single insertion after reconfiguration,
font bitmaps, the message-byte boundary and the Settings control. Preference
checks cover upgrades, stored/invalid choices and failed writes/commits.
The tagged release records the exact package and physical acceptance.

Localization, structured cross-client replies, optional sensors and the wider
WadaMesh app suite remain on the existing roadmap.
