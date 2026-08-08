# DeskOS D1L 1.0 production release

This procedure turns one successful `main` build into the public DeskOS 1.0
downloads. It does not require a controlled peer, Wi-Fi credential, admin
password, soak run, physical receipt, or release-audit script.

End users should not follow this maintainer procedure. They download the ZIP,
extract it, and follow `START_HERE.md` inside it.

## 1. Select the exact main build

Run on the Pi from a clean checkout:

```bash
set -euo pipefail

export ROOT=<absolute-clean-checkout>
export SHA=<40-character-main-commit>
export RUN=<successful-main-push-run-id>
export PORT=/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
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
test -f "$PACKAGE/START_HERE.md"
```

## 3. Install through the end-user path

Use the package itself, not an internal hardware runner:

```bash
test -L "$PORT" && test -r "$PORT" && test -w "$PORT"
D1L_PORT="$PORT" "$PACKAGE/flash_project.sh"
```

This is the normal non-erasing ESP32 installation documented for users. It
writes the packaged bootloader, partition table, OTA selection data, and app
without issuing an erase. The RP2040 UF2 is installed only through the physical
BOOTSEL/UF2 procedure in `START_HERE.md`.

After boot, use DeskOS normally: complete setup if needed, open Home, Channels,
Contacts, Map, and Settings, and send a Public message on the available mesh.
This is ordinary product use, not a controlled-peer acceptance procedure.

## 4. Stage the public downloads

Use one asset set for both RC1 and stable so the bytes are identical:

```bash
ASSET_DIR="$ROOT/artifacts/DeskOS-D1L-1.0.0"
ZIP_STAGE="$ROOT/artifacts/DeskOS-D1L-1.0.0-zip"
test ! -e "$ASSET_DIR"
test ! -e "$ZIP_STAGE"
mkdir -p "$ASSET_DIR"
mkdir -p "$ZIP_STAGE"

cp -a "$PACKAGE" "$ZIP_STAGE/MeshCore-DeskOS-D1L-1.0.0"
(
  cd "$ZIP_STAGE"
  python3 -m zipfile -c \
    "$ASSET_DIR/MeshCore-DeskOS-D1L-1.0.0.zip" \
    MeshCore-DeskOS-D1L-1.0.0
)

cp "$PACKAGE/firmware/bootloader.bin" \
  "$ASSET_DIR/MeshCore-DeskOS-D1L-1.0.0-bootloader.bin"
cp "$PACKAGE/firmware/partition-table.bin" \
  "$ASSET_DIR/MeshCore-DeskOS-D1L-1.0.0-partition-table.bin"
cp "$PACKAGE/firmware/ota_data_initial.bin" \
  "$ASSET_DIR/MeshCore-DeskOS-D1L-1.0.0-ota-data.bin"
cp "$PACKAGE/firmware/meshcore_deskos_d1l.bin" \
  "$ASSET_DIR/MeshCore-DeskOS-D1L-1.0.0-app.bin"
cp "$PACKAGE/full-flash/meshcore_deskos_d1l-full-8mb.bin" \
  "$ASSET_DIR/MeshCore-DeskOS-D1L-1.0.0-full-8mb.bin"
cp "$PACKAGE/rp2040/rp2040-sd-bridge-firmware/deskos_sd_bridge.ino.uf2" \
  "$ASSET_DIR/MeshCore-DeskOS-D1L-1.0.0-rp2040-sd-bridge.uf2"
cp "$PACKAGE/START_HERE.md" "$ASSET_DIR/START_HERE-1.0.0.md"

(cd "$ASSET_DIR" && sha256sum \
  MeshCore-DeskOS-D1L-1.0.0.zip \
  MeshCore-DeskOS-D1L-1.0.0-bootloader.bin \
  MeshCore-DeskOS-D1L-1.0.0-partition-table.bin \
  MeshCore-DeskOS-D1L-1.0.0-ota-data.bin \
  MeshCore-DeskOS-D1L-1.0.0-app.bin \
  MeshCore-DeskOS-D1L-1.0.0-full-8mb.bin \
  MeshCore-DeskOS-D1L-1.0.0-rp2040-sd-bridge.uf2 \
  START_HERE-1.0.0.md > SHA256SUMS-1.0.0.txt)
```

The ZIP is the recommended user download. The standalone app/bootloader/
partition/OTA files are for experienced ESP-IDF/esptool users. The full 8 MB
image is factory recovery and can overwrite retained state.

## 5. Publish RC1

```bash
test -z "$(git ls-remote --tags origin refs/tags/v1.0.0-rc.1)"
git tag -a v1.0.0-rc.1 "$SHA" -m "MeshCore DeskOS D1L 1.0.0 RC1"
git push origin refs/tags/v1.0.0-rc.1

gh release create v1.0.0-rc.1 "$ASSET_DIR"/* \
  --repo "$REPO" \
  --verify-tag \
  --target "$SHA" \
  --title "MeshCore DeskOS D1L 1.0.0 RC1" \
  --prerelease \
  --latest=false \
  --notes "Production DeskOS 1.0 RC1 for SenseCAP Indicator D1L. Download the ZIP, extract it fully, and start with START_HERE.md."
```

## 6. Publish the same bytes as stable 1.0

Do not rebuild or restage between RC1 and stable:

```bash
test "$(git rev-parse 'v1.0.0-rc.1^{commit}')" = "$SHA"
test -z "$(git ls-remote --tags origin refs/tags/v1.0.0)"

git tag -a v1.0.0 "$SHA" -m "MeshCore DeskOS D1L 1.0.0"
git push origin refs/tags/v1.0.0

gh release create v1.0.0 "$ASSET_DIR"/* \
  --repo "$REPO" \
  --verify-tag \
  --target "$SHA" \
  --title "MeshCore DeskOS D1L 1.0.0" \
  --latest \
  --notes "Stable DeskOS 1.0 for SenseCAP Indicator D1L. Download the ZIP, extract it fully, and start with START_HERE.md."
```

## 7. Confirm the public handoff

Download stable into a new directory and compare it with the staged assets:

```bash
VERIFY_DIR="$(mktemp -d)"
gh release download v1.0.0 --repo "$REPO" --dir "$VERIFY_DIR"
(cd "$VERIFY_DIR" && sha256sum --check SHA256SUMS-1.0.0.txt)

for file in "$ASSET_DIR"/*; do
  cmp --silent "$file" "$VERIFY_DIR/$(basename "$file")"
done
```

Then confirm the release page tells users to download the ZIP and open
`START_HERE.md`. No private credentials or project-operated mesh peer are part
of the public handoff.
