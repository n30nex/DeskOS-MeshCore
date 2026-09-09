# DeskOS D1L release execution

## Current local release procedure: 1.8.0-rc.5

The maintainer requested local builds on the Pi 5. The 1.0 procedure below is
historical and must not trigger Actions for the current candidate.

1. Freeze a clean source commit and its pinned submodules in an isolated Pi
   checkout. Run the complete host suite and the checks required by the changed
   firmware and installation paths.
2. Use ESP-IDF v5.5.4, pinned by `.github/d1l-build-inputs.json`, and configure
   `D1L_RELEASE_PROFILE=full_feature`, `D1L_SD_HISTORY_MODE=conditional`.
   The normal CMake build applies the four reviewed BSP patches. Keep the build
   and Python temporary directories inside the task workspace.
   A native Pi build may use the official v5.5.4 source at
   `735507283d5b2f9fb363a1901172dbd9e847945d` and its checksum-verified
   `install.sh esp32s3` tools in an isolated `IDF_TOOLS_PATH`. Record the
   native SDK commit and actual tool versions; do not label it a container
   or Actions build. This avoids installing SDK tools for unrelated targets.
3. Run the existing `scripts/package_release_d1l.py` local-release path. Include
   the complete production bridge UF2, preserving app update and boot selector,
   clean 8 MB image, signed update, provenance, SBOM, instructions and checksums.
   An unchanged bridge may be reused only with verified source/tool inputs and
   matching artifact hashes. Keep its original build provenance accurate.
4. Verify the package using its own `scripts/verify_package.py`. Resolve the
   attached D1L's stable by-id path and `1a86:7523`, retain a private recovery
   copy, and use the preserving installer at its declared offsets. Verify the
   new application before writing its boot selector. Never format SD.
5. Verify the exact running version/commit, identity, display, radio and
   retained storage. Exercise the changed behavior using production firmware.
   Record any physical update/phone paths that were not exercised explicitly.
6. Merge the tested source, tag `v1.8.0-rc.5`, and publish it as a prerelease.
   Freshly download every asset and compare its bytes with staging. Update the
   existing flasher catalog and Canadaverse DeskOS page, then verify their
   public content and downloads. Remove obsolete build outputs only after
   retaining release and recovery files.

Use [build provenance](BUILD_PROVENANCE_D1L.md) for the existing package options
and [the release checklist](RELEASE_CHECKLIST.md) for candidate acceptance.
Signing credentials stay in the local credential store, never in the source or
public package. Local test results must not be represented as Actions results.

## Historical 1.0 production release

This procedure turns one successful `main` build into the public DeskOS 1.0.1
downloads. It does not require a controlled peer, Wi-Fi credential, admin
password, soak run, physical receipt, or release-audit script.

End users should not follow this maintainer procedure. They download the ZIP,
extract it, and follow `START_HERE.md` inside it.

The project maintainer's attached D1L is
`/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`; public users select their own
device port from `START_HERE.md`.

Map and storage behavior are ordinary product use, not release-lab inputs.

## 1. Select the exact main build

Run on the Pi from a clean checkout:

```bash
set -euo pipefail

export ROOT=<absolute-clean-checkout>
export SHA=<40-character-main-commit>
export RUN=<successful-main-push-run-id>
export REPO=n30nex/DeskOS-MeshCore

cd "$ROOT"
git fetch origin main --tags
test "$(git rev-parse origin/main)" = "$SHA"
git checkout --detach "$SHA"
test -z "$(git status --porcelain=v1)"

gh run view "$RUN" --repo "$REPO" --exit-status
test "$(gh run view "$RUN" --repo "$REPO" --json headSha --jq .headSha)" = "$SHA"
test "$(gh run view "$RUN" --repo "$REPO" --json event --jq .event)" = push
test "$(gh run view "$RUN" --repo "$REPO" --json headBranch --jq .headBranch)" = main
```

## 2. Download the production package

```bash
RUN_DIR="$ROOT/artifacts/release-run-$RUN"
test ! -e "$RUN_DIR"
mkdir -p "$RUN_DIR"

gh run download "$RUN" \
  --repo "$REPO" \
  --name d1l-release-package \
  --dir "$RUN_DIR"

PACKAGE="$RUN_DIR/d1l-release-$SHA"
test -f "$PACKAGE/manifest.json"
test -f "$PACKAGE/SHA256SUMS.txt"
python3 "$PACKAGE/scripts/verify_package.py" "$PACKAGE"
```

The required public files are:

```bash
test -f "$PACKAGE/firmware/bootloader.bin"
test -f "$PACKAGE/firmware/partition-table.bin"
test -f "$PACKAGE/firmware/ota_data_initial.bin"
test -f "$PACKAGE/firmware/meshcore_deskos_d1l.bin"
test -f "$PACKAGE/full-flash/meshcore_deskos_d1l-full-8mb.bin"
test -f "$PACKAGE/rp2040/rp2040-sd-bridge-firmware/deskos_sd_bridge.ino.uf2"
test -f "$PACKAGE/flash_update_bin.ps1"
test -f "$PACKAGE/flash_update_bin.sh"
test -f "$PACKAGE/flash_full_8mb.ps1"
test -f "$PACKAGE/flash_full_8mb.sh"
test -f "$PACKAGE/START_HERE.md"
```

## 3. Confirm the two public install paths

Do not run a release-validation firmware or controlled-peer workflow. The public
package itself must expose both end-user paths:

```bash
grep -F 'Existing DeskOS: preserving update BIN' "$PACKAGE/START_HERE.md"
grep -F 'No DeskOS: full clean 8 MB BIN' "$PACKAGE/START_HERE.md"
grep -F 'same production UF2' "$PACKAGE/START_HERE.md"
```

`flash_update_bin.*` writes the app BIN at its declared offset for an existing
DeskOS device. `flash_full_8mb.*` writes the complete image at `0x0` for a
blank/non-DeskOS device. `flash_rp2040.*` installs the same complete bridge UF2
for either path.

## 4. Stage the public downloads

Stage one clearly named stable asset set:

```bash
VERSION=1.0.1
ASSET_DIR="$ROOT/artifacts/DeskOS-D1L-$VERSION"
ZIP_STAGE="$ROOT/artifacts/DeskOS-D1L-$VERSION-zip"
test ! -e "$ASSET_DIR"
test ! -e "$ZIP_STAGE"
mkdir -p "$ASSET_DIR"
mkdir -p "$ZIP_STAGE"

cp -a "$PACKAGE" "$ZIP_STAGE/MeshCore-DeskOS-D1L-$VERSION"
(
  cd "$ZIP_STAGE"
  python3 -m zipfile -c \
    "$ASSET_DIR/MeshCore-DeskOS-D1L-$VERSION.zip" \
    "MeshCore-DeskOS-D1L-$VERSION"
)

cp "$PACKAGE/firmware/bootloader.bin" \
  "$ASSET_DIR/MeshCore-DeskOS-D1L-$VERSION-bootloader.bin"
cp "$PACKAGE/firmware/partition-table.bin" \
  "$ASSET_DIR/MeshCore-DeskOS-D1L-$VERSION-partition-table.bin"
cp "$PACKAGE/firmware/ota_data_initial.bin" \
  "$ASSET_DIR/MeshCore-DeskOS-D1L-$VERSION-ota-data.bin"
cp "$PACKAGE/firmware/meshcore_deskos_d1l.bin" \
  "$ASSET_DIR/MeshCore-DeskOS-D1L-$VERSION-UPDATE-existing-deskos-at-0x20000.bin"
cp "$PACKAGE/full-flash/meshcore_deskos_d1l-full-8mb.bin" \
  "$ASSET_DIR/MeshCore-DeskOS-D1L-$VERSION-FRESH-CLEAN-full-8mb-at-0x0.bin"
cp "$PACKAGE/rp2040/rp2040-sd-bridge-firmware/deskos_sd_bridge.ino.uf2" \
  "$ASSET_DIR/MeshCore-DeskOS-D1L-$VERSION-RP2040-UPDATE-OR-FRESH.uf2"
cp "$PACKAGE/START_HERE.md" "$ASSET_DIR/START_HERE-$VERSION.md"

(cd "$ASSET_DIR" && sha256sum \
  "MeshCore-DeskOS-D1L-$VERSION.zip" \
  "MeshCore-DeskOS-D1L-$VERSION-bootloader.bin" \
  "MeshCore-DeskOS-D1L-$VERSION-partition-table.bin" \
  "MeshCore-DeskOS-D1L-$VERSION-ota-data.bin" \
  "MeshCore-DeskOS-D1L-$VERSION-UPDATE-existing-deskos-at-0x20000.bin" \
  "MeshCore-DeskOS-D1L-$VERSION-FRESH-CLEAN-full-8mb-at-0x0.bin" \
  "MeshCore-DeskOS-D1L-$VERSION-RP2040-UPDATE-OR-FRESH.uf2" \
  "START_HERE-$VERSION.md" > "SHA256SUMS-$VERSION.txt")
```

The ZIP is the recommended user download. The standalone update BIN is only for
an existing DeskOS device at `0x20000`. The standalone full clean BIN is for a
blank/non-DeskOS device at `0x0` and removes all prior ESP32 data. The one
RP2040 UF2 installs the complete bridge firmware for either path.

## 5. Publish stable 1.0.1

```bash
TAG=v1.0.1
test -z "$(git ls-remote --tags origin "refs/tags/$TAG")"
git tag -a "$TAG" "$SHA" -m "MeshCore DeskOS D1L $VERSION"
git push origin "refs/tags/$TAG"

gh release create "$TAG" "$ASSET_DIR"/* \
  --repo "$REPO" \
  --verify-tag \
  --target "$SHA" \
  --title "MeshCore DeskOS D1L $VERSION" \
  --latest \
  --notes "Stable DeskOS $VERSION for SenseCAP Indicator D1L. Download the ZIP and choose UPDATE EXISTING DESKOS or FRESH CLEAN INSTALL in START_HERE-$VERSION.md. The same complete RP2040 UF2 is used for either path."
```

## 6. Confirm the public handoff

Download stable into a new directory and compare it with the staged assets:

```bash
VERIFY_DIR="$(mktemp -d)"
gh release download "$TAG" --repo "$REPO" --dir "$VERIFY_DIR"
(cd "$VERIFY_DIR" && sha256sum --check "SHA256SUMS-$VERSION.txt")

for file in "$ASSET_DIR"/*; do
  cmp --silent "$file" "$VERIFY_DIR/$(basename "$file")"
done
```

Then confirm the release page tells users to download the ZIP and open
`START_HERE-1.0.1.md`. No private credentials or project-operated mesh peer are part
of the public handoff.
