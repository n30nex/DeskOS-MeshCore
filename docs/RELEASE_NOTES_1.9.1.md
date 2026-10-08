# DeskOS D1L 1.9.1

This maintenance release addresses SD save latency. Messages, drafts, contacts,
nodes, routes and retained packet segments share one verified transfer per
saved file, using the streaming operation already supported by the production
RP2040 bridge. It replaces repeated open/flush/close cycles for 192-byte writes.

The bridge flushes and reads back the complete temporary file, checking its
size and CRC before the existing replace-rename commits it. A failed transfer,
readback or cancellation never reports a saved primary. Card-generation and
identity-lineage checks, foreground cancellation and previous-copy recovery
remain in place. Failed cancellation cleanup is reported as a storage error.

The saved data format, identity/settings envelope, signed updater and RP2040
firmware are unchanged. Use the paired production bridge included in the
release package. Preserving USB updates keep identity, contacts, settings and
SD data; firmware never formats the card.

Native regression checks inject a card-byte error after an accepted write and
verify that the previous saved copy survives. They also cover successful retry,
mid-save cancellation, failed abort cleanup and backend-generation changes.
The tagged release records exact firmware, build and device measurements;
earlier phone-app or SD-update runs are not new-candidate hardware acceptance.

The 1.9 features and [parity limits](DESKOS_MESHCORE_FEATURE_PARITY.md) continue
to apply. Slow or unavailable cards remain visible. Companion delivery is
confirmed only after the acknowledgement is saved, and remote responses depend
on the peer and route.
