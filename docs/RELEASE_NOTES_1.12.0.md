# DeskOS D1L 1.12.0

Startup SD saves now allow for cold card writes that exceed the previous
750 ms deadline. Retained writes and packet-journal writes use a bounded
three-second budget. The shared file reader matches each response to its
request ID, so a late response cannot be mistaken for the following cleanup
or save. Read deadlines and urgent reboot cleanup remain bounded separately.

CRC checks, complete temporary-file verification, media-generation guards,
atomic replacement and truthful error counters remain in place. The firmware
does not format the card or reset failure counters to make a run look healthy.

**Settings / Messaging / Notifications / Presentation** enables a larger
receive view for demonstrations. It shows the sender and message text for
both channels and DMs in 24-pixel text, including the existing Latin accents.
The card pulses and slides into view, stays for about 6.5 seconds, and can be
dismissed with a tap. A newer message replaces the preview; conversations
retain their normal history. There is no growing animation queue.

Presentation mode keeps the screen awake and is saved across restarts. Home
shows that the mode is on. **It exposes DM and private-channel text on the
display.** Turn it off after a public demonstration. Manual Lock, muted
contacts, Visual Alerts Off and quiet hours still suppress notifications.
Dialogs and keyboards retain compact alerts so their controls remain usable.

Normal mode keeps the private 2.2-second header animation. Receive alerts now
use a small, coherent snapshot every 100 ms instead of waiting for the full
screen model refresh. Neither mode changes drafts, sends messages, marks
conversations read, or animates restored history and rejected duplicates.

Existing installations use the preserving USB update or signed SD package.
The RP2040 bridge protocol and firmware are unchanged. Exact builds, restart
tests, radio reception and public-download checks are recorded in the tagged
release; the existing feature-parity limits still apply.
