# DeskOS D1L 1.9.2

Packet history filtering and search now run outside the touchscreen task.
Previously a filter could scan the complete SD archive to count matches while
navigation waited. Pages now hold 12 rows and fetch one extra match to decide
whether Older is available. A `+` beside the range means more matches exist;
it is not an exact total.

Loading and card errors are visible. Changing filters, applying a new search,
pausing or leaving Packets cancels the old request. Results from a cleared
history or changed card are discarded. Slow cards can still delay results,
but the UI no longer waits for that scan to finish. Tap a filter to retry an
unavailable search.

The storage format, identity/settings envelope, Bluetooth protocol, signed
updater and RP2040 firmware are unchanged. The preserving USB update keeps
identity, contacts, settings and card data. Firmware never formats the card.

Native checks cover bounded page reads, cancellation during a scan, request
replacement, live-update coalescing, input ownership, card/history changes,
allocation failure and SD read errors. The tagged release records the exact
Actions package, local Pi build and physical checks. Earlier official phone
or signed-SD installation results are not new-candidate hardware acceptance.

The existing [parity limits](DESKOS_MESHCORE_FEATURE_PARITY.md) still apply.
