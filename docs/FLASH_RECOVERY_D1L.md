# Full Feature Install and Recovery

> **SUPERSEDED INSTALL/RECOVERY HISTORY — NOT CURRENT RC1 INSTRUCTIONS.**
> Signed local-SD update and rollback shipped in 1.5; this historical guide is
> still superseded by the exact current release package.
> Do not execute the historical package, port, recovery or soak directions
> below. Use the exact downloaded GitHub Actions package README together with
> the [project README](../README.md), [RC1 user guide](USER_GUIDE_D1L.md),
> [RC1 test plan](TEST_PLAN_D1L.md), and
> [SD-card guide](D1L_SD_CARD_GUIDED_INSTALL.md).

This repository guide applies only to an extracted MeshCore DeskOS D1L Full
Feature release package. Build firmware only in GitHub Actions and use the exact
package whose commit and Actions run match the release candidate.

## Safety rules

- Verify every package file against the package-root `SHA256SUMS.txt` before
  running either flash helper.
- The current release-closing D1L is on `neopi5`, accessed through the
  unprivileged, key-only `siguidev` account. Use only
  `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0` and require USB identity
  `1A86:7523` before opening or flashing it.
- The current `/dev/ttyUSB2` resolution is observational only. Never pass a
  raw `/dev/ttyUSB*` name to a release command.
- The old Windows COM assignment is stale. There is no current Windows
  alternative release route.
- Never format SD.
- A normal install is non-erasing. Do not use the full-flash helper for an
  update.
- Finding the device or moving it to `neopi5` does not qualify a release. The
  exact-SHA flash and all UI, reboot, protocol, RF, soak, manual-review, and
  final-audit gates remain fail-closed.

## Verify the extracted package

Run this from the extracted package root:

```powershell
$ErrorActionPreference = "Stop"
Get-Content .\SHA256SUMS.txt | ForEach-Object {
    if ($_ -notmatch '^([0-9a-f]{64})  \./(.+)$') {
        throw "Invalid SHA256SUMS.txt row: $_"
    }
    $expected = $Matches[1]
    $path = Join-Path (Get-Location) $Matches[2]
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        throw "Checksum mismatch: $path"
    }
}
```

Stop if any checksum, expected commit, Actions run, release profile, or SD mode
does not match the candidate being released.

## Normal non-erasing install

The extracted package supplies the only supported normal-install helpers.
For the current Pi 5 route, log in with the key-only development account and
run from the extracted, exact-candidate package directory:

```bash
ssh siguidev@neopi5
cd /path/to/extracted-package-directory
sha256sum --check SHA256SUMS.txt
export D1L_PORT='/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
test -L "$D1L_PORT" && test -r "$D1L_PORT" && test -w "$D1L_PORT"
D1L_DEVICE_PROPERTIES="$(udevadm info --query=property --name="$D1L_PORT")"
grep -qx 'ID_VENDOR_ID=1a86' <<<"$D1L_DEVICE_PROPERTIES"
grep -qx 'ID_MODEL_ID=7523' <<<"$D1L_DEVICE_PROPERTIES"
./flash_project.sh
```

Stop unless both expected USB properties are present. The symlink may resolve
through a different `/dev/ttyUSB*` number after a move or reboot; that is why
only the stable by-id path is passed to the helper.

Do not substitute a repository build directory, `idf.py flash`, a predecessor
artifact, an unverified binary, or another serial device.

## Destructive recovery only

Use `flash_full_8mb.ps1` only when normal install cannot recover the device and
a destructive recovery has been separately authorized. It writes the full
8 MB image and can overwrite settings, contacts, messages, and logs. The
current package has no authorized Pi destructive-recovery helper; do not
improvise one against the by-id device. Stop and establish a separately
reviewed exact-target recovery route before attempting a full 8 MB write.
