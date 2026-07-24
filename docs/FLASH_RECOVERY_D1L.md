# Core 1.0 Install and Recovery

This repository guide applies only to an extracted MeshCore DeskOS D1L Core
1.0 release package. Build firmware only in GitHub Actions and use the exact
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
- `COM12` remains the valid Windows alternative for the D1L.
- `COM16` is reserved for separately authorized SD/RP2040 work, is not needed
  by the Core package, and is never the Core D1L target.
- Never use COM8, COM11, or COM29.
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

When the D1L is intentionally moved back to the Windows host, `COM12` remains
the valid alternative:

```powershell
$env:D1L_PORT = "COM12"
.\flash_project.ps1 -Port $env:D1L_PORT
```

Do not substitute a repository build directory, `idf.py flash`, a predecessor
artifact, an unverified binary, or another serial device.

## Destructive recovery only

Use `flash_full_8mb.ps1` only when normal install cannot recover the device and
a destructive recovery has been separately authorized. It writes the full
8 MB image and can overwrite settings, contacts, messages, and logs. The
current package has no authorized Pi destructive-recovery helper; do not
improvise one against the by-id device. If the D1L is intentionally moved to
the Windows recovery route, confirm that a recoverable backup exists when
possible, re-verify the package checksums, and run only against `COM12`:

```powershell
$env:D1L_PORT = "COM12"
.\flash_full_8mb.ps1 -Port $env:D1L_PORT
```

The helper requires the typed confirmation `FULL-FLASH-COM12`. Cancel if the
port or confirmation text differs.
