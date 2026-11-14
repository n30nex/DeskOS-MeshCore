# DeskOS D1L 1.11.0

Newly received messages display an animated envelope banner in the header for
about 2.2 seconds. It slides in, gently pulses, and slides out. Cyan identifies
group messages (Public, any other enabled channel, or an authenticated room
post); lime identifies a direct message. A mixed burst uses a gold banner.

The banner is non-interactive and passes touches through. It does not switch
screens, move the text cursor, clear drafts, or reveal message text or sender
names. Bursts share one bounded animation instead of creating a growing queue
or extending a banner indefinitely.

Notifications now follow new message admissions rather than changes in unread
counts. This covers private channels and messages arriving while their chat is
open, without replaying old history after startup or generating notifications
for transmitted messages, duplicates or rejected packets.

**Settings / Messaging / Notifications / Cycle Alerts** controls both the
animation and existing backlight pulse. Off disables them; quiet hours suppress
them from 22:00 to 07:00 using the display clock. An unavailable clock stays
quiet in that mode. Muted contacts remain quiet, and the firmware does not wake
a locked or sleeping screen for an animation.

Message storage formats, identity, channel keys, radio settings and the signed
updater are unchanged. Use the preserving USB update for an existing DeskOS
installation. Exact build and device acceptance are recorded in the tagged
release; the existing parity limits continue to apply.

Build verification now carries the unchanged, checksum-pinned Crypto host-test
archive locally after its registry retired direct downloads. Firmware
cryptography is unchanged.
