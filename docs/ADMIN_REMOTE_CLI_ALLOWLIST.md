# Authenticated remote CLI allowlist

This is the Core 1.0 / RC1 allowlist for commands sent to a compatible
authenticated MeshCore repeater or room server. Remote CLI requires admin
permission. Commands are case-sensitive and must use the lowercase spelling
shown here.

Read-only commands send immediately:

- both roles: `ver`, `board`, `clock`, `gps`, `gps advert`, `region`,
  `region home`, `region default`, `region get <name>`,
  `region list allowed`, `region list denied`, `sensor list`,
  `sensor list <offset>`, and `sensor get <key>`;
- repeater only: `neighbors` and `powersaving`;
- `get <key>` for the allowed settings below.

Mutations require a second local confirmation:

- both roles: `clock sync`, `advert`, `advert.zerohop`, `clear stats`,
  `log start`, `log stop`, `log erase`, `gps on`, `gps off`, `gps sync`,
  `gps setloc <args>`, `gps advert none`, `gps advert share`,
  `gps advert prefs`, `region save`, `region allowf <args>`,
  `region denyf <args>`, `region put <args>`, `region def <args>`,
  `region remove <args>`, `region home <args>`,
  `region default <args>`, `time <seconds>`, `tempradio <args>`,
  `sensor set <key> <value>`, and `setperm <64-hex-key> <0|1|2|3>`;
- repeater only: `discover.neighbors`, `powersaving on`,
  `powersaving off`, and `neighbor.remove <hex-key>`;
- `set <key> <value>` for writable settings below.

The guided access-list editor uses permission `0` to remove an entry, `1` for
read, `2` for write, and `3` for admin. Add, update, and remove all require the
full 64-hex public key. Room guest-reading controls use
`set allow.read.only on|off`.

## Setting keys

These non-sensitive keys support both `get` and `set` on both roles:

`dutycycle`, `af`, `int.thresh`, `agc.reset.interval`, `multi.acks`,
`flood.advert.interval`, `advert.interval`, `name`, `repeat`,
`radio.rxgain`, `radio`, `lat`, `lon`, `rxdelay`, `txdelay`,
`flood.max.unscoped`, `flood.max.advert`, `flood.max`, `direct.txdelay`,
`owner.info`, `path.hash.mode`, `loop.detect`, `tx`, `bridge.enabled`,
`bridge.delay`, `bridge.source`, `bridge.baud`, `bridge.channel`, and
`adc.multiplier`.

Role-specific and restricted settings:

- room only, read/write: `allow.read.only`;
- both roles, read/write but sensitive: `guest.password`, `bridge.secret`;
- both roles, set-only and sensitive: `prv.key`;
- both roles, read-only: `freq`, `public.key`, `role`, `bridge.type`,
  `bootloader.ver`, `pwrmgt.support`, `pwrmgt.source`,
  `pwrmgt.bootreason`, and `pwrmgt.bootmv`.

The direct `password <value>` command is also allowed only as a sensitive,
locally confirmed on-device command.

## USB wrapper

- Read: `admin cli <documented-command>`
- Change: `admin cli <documented-command> CONFIRM-REMOTE-MUTATION`
- Room post: `admin room-post <text>`

Sensitive commands are rejected by the USB wrapper and must be entered through
the on-device **Secure Input** control. Command text is never echoed in the USB
result.

Serial-only commands, unknown commands, OTA, reboot, shutdown/poweroff,
private-key reads, and frequency writes are not available through the remote
CLI. They fail closed instead of being sent to a peer.
