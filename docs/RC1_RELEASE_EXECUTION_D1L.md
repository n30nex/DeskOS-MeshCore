# DeskOS 1.0 RC1 release execution

This is the maintainer closing procedure for `v1.0.0-rc.1`. It uses one exact
successful `main` push, its downloaded package, one non-erasing flash, four
machine sources, one aggregate, and one fail-closed audit. It is not an
ordinary user installation guide.

## 1. Preconditions

Work from one fresh, clean checkout on the authorized Pi host. Set
operator-specific values without putting credentials in Git or command-line
arguments:

```bash
export ROOT=<absolute-clean-checkout>
export PY=<absolute-python-path>
export SHA=<40-character-main-commit>
export RUN=<successful-main-push-run-id>
export ATTEMPT=<run-attempt>
export PORT=/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
export D1L_PUBLIC_KEY=<confirmed-current-64-hex-public-key>
export PEER_STATUS=<absolute-controlled-peer-status-path>
export PEER_CONTROL_SOCKET=<absolute-controlled-peer-socket-path>
export PEER_DEVICE=<controlled-peer-device-identity>
export PEER_PUBLIC_KEY=<controlled-peer-64-hex-public-key>
export PEER_FINGERPRINT=<controlled-peer-fingerprint>
export PEER_SERVICE=<controlled-peer-service>
export PEER_STATUS_SCHEMA=meshcorebot_v1
export ADMIN_FINGERPRINT=<authorized-repeater-fingerprint>
export ADMIN_PASSWORD_FILE=<absolute-password-file-outside-repository>

test -x "$PY"
test "${#SHA}" -eq 40
test "${#D1L_PUBLIC_KEY}" -eq 64
test -f "$ADMIN_PASSWORD_FILE"
```

The device must already have a prepared FAT32 card, configured location and
Wi-Fi, an HTTPS Map provider manifest permitting offline storage and background
prefetch, and the controlled peer/admin inputs. Never format SD or substitute
an unknown peer.

## 2. Candidate identity capture

```bash
cd "$ROOT"
git fetch origin main --tags
test "$(git rev-parse origin/main)" = "$SHA"
git checkout --detach "$SHA"
test "$(git rev-parse HEAD)" = "$SHA"
test -z "$(git status --porcelain=v1)"

gh run view "$RUN" --repo n30nex/SIGUI --exit-status
test "$(gh run view "$RUN" --repo n30nex/SIGUI --json headSha --jq .headSha)" = "$SHA"
test "$(gh run view "$RUN" --repo n30nex/SIGUI --json event --jq .event)" = push
test "$(gh run view "$RUN" --repo n30nex/SIGUI --json headBranch --jq .headBranch)" = main
test "$(gh run view "$RUN" --repo n30nex/SIGUI --json attempt --jq .attempt)" = "$ATTEMPT"
```

## 3. Artifact download and checksum verification

The capture command requires the exact eight RC1 artifacts and refuses stale or
mismatched run identity.

```bash
"$PY" scripts/capture_core_actions_run_d1l.py \
  --root "$ROOT" \
  --github-run-id "$RUN" \
  --commit "$SHA"

RUN_DIR="$ROOT/artifacts/github/$RUN"
CAPTURE="$RUN_DIR/core-actions-run-metadata/core_actions_run_${RUN}.json"
PACKAGE="$RUN_DIR/d1l-release-package/d1l-release-${SHA}"

test -f "$CAPTURE"
test -f "$PACKAGE/manifest.json"
(cd "$PACKAGE" && sha256sum --check SHA256SUMS.txt)
```

## 4. Stable target admission

```bash
test -L "$PORT" && test -r "$PORT" && test -w "$PORT"
D1L_PROPERTIES="$(udevadm info --query=property --name="$PORT")"
grep -qx 'ID_VENDOR_ID=1a86' <<<"$D1L_PROPERTIES"
grep -qx 'ID_MODEL_ID=7523' <<<"$D1L_PROPERTIES"
```

Do not use `/dev/ttyUSB*`, guess a Windows COM port, or probe another serial
device.

## 5. Non-erasing flash

```bash
EVIDENCE_DIR="$ROOT/artifacts/rc1-final/$SHA"
mkdir -p "$EVIDENCE_DIR"
FLASH="$EVIDENCE_DIR/flash-retained-reflash.json"

"$PY" scripts/core_flash_only_d1l.py \
  --root "$ROOT" \
  --github-run-id "$RUN" \
  --github-run-attempt "$ATTEMPT" \
  --github-run-dir "$RUN_DIR" \
  --package-dir "$PACKAGE" \
  --actions-capture-receipt "$CAPTURE" \
  --commit "$SHA" \
  --port "$PORT" \
  --expected-d1l-public-key "$D1L_PUBLIC_KEY" \
  --serial-timeout 60 \
  --settle-sec 90 \
  --phase retained-reflash \
  --out "$FLASH"
```

This source must prove the exact app flash, stable identity, no erase or SD
format, and preserved settings. A failed source stops the run.

## 6. Four source commands

The flash above is source R2. Run Map before the RF and protocol sources because
Map performs a normal reboot. Protocol is last because it authorizes the one
bounded Public send.

```bash
MAP="$EVIDENCE_DIR/map-acceptance.json"
"$PY" scripts/rc1_map_acceptance_d1l.py \
  --root "$ROOT" \
  --port "$PORT" \
  --expected-firmware-commit "$SHA" \
  --github-actions-run "$RUN" \
  --workflow-run-attempt "$ATTEMPT" \
  --output "$MAP"

RF="$EVIDENCE_DIR/rf-full-acceptance.json"
"$PY" scripts/rf_full_acceptance_d1l.py \
  --port "$PORT" \
  --baud 115200 \
  --peer-status "$PEER_STATUS" \
  --peer-port "$PEER_DEVICE" \
  --fingerprint "$PEER_FINGERPRINT" \
  --d1l-public-key "$D1L_PUBLIC_KEY" \
  --token "rc1-${SHA:0:8}" \
  --timeout 120 \
  --wait-sec 300 \
  --poll-sec 3 \
  --commit "$SHA" \
  --github-run-id "$RUN" \
  --github-run-attempt "$ATTEMPT" \
  --out "$RF"

PROTOCOL="$EVIDENCE_DIR/protocol-admin.json"
"$PY" scripts/produce_rc1_protocol_acceptance_d1l.py \
  --root "$ROOT" \
  --output "$PROTOCOL" \
  --commit "$SHA" \
  --github-run-id "$RUN" \
  --github-run-attempt "$ATTEMPT" \
  --peer-status "$PEER_STATUS" \
  --peer-control-socket "$PEER_CONTROL_SOCKET" \
  --peer-public-key "$PEER_PUBLIC_KEY" \
  --peer-device "$PEER_DEVICE" \
  --peer-service "$PEER_SERVICE" \
  --peer-status-schema "$PEER_STATUS_SCHEMA" \
  --admin-fingerprint "$ADMIN_FINGERPRINT" \
  --admin-password-file "$ADMIN_PASSWORD_FILE" \
  --boot-timeout 75 \
  --command-timeout 60 \
  --authorize-public-tx
```

## 7. Evidence aggregation

```bash
PHYSICAL="$EVIDENCE_DIR/rc1-bounded-physical-${SHA}.json"
PHYSICAL_EVIDENCE="$EVIDENCE_DIR/rc1-bounded-physical-${SHA}.evidence.json"

"$PY" scripts/produce_rc1_bounded_physical_receipt_d1l.py \
  --package-dir "$PACKAGE" \
  --evidence-root "$ROOT" \
  --flash-receipt "$FLASH" \
  --rf-receipt "$RF" \
  --protocol-receipt "$PROTOCOL" \
  --map-receipt "$MAP" \
  --output "$PHYSICAL" \
  --evidence-output "$PHYSICAL_EVIDENCE"
```

## 8. RC1 audit command

```bash
AUDIT="$EVIDENCE_DIR/rc1-release-audit-${SHA}.json"
"$PY" scripts/rc1_release_gate_audit_d1l.py \
  --root "$ROOT" \
  --package-dir "$PACKAGE" \
  --actions-receipt "$CAPTURE" \
  --physical-receipt "$PHYSICAL" \
  --physical-evidence "$PHYSICAL_EVIDENCE" \
  --output "$AUDIT"

test "$(jq -r .ready_for_public_release "$AUDIT")" = true
```

## 9. Prerelease publication

Only after the audit is true, stage the production ZIP plus its outer checksum,
create an annotated `v1.0.0-rc.1` tag at `$SHA`, and publish a prerelease.
Stable `v1.0.0` remains untouched.

```bash
RELEASE_DIR="$ROOT/artifacts/release/v1.0.0-rc.1"
test ! -e "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"
mapfile -t PACKAGE_ARCHIVES < <(find "$RUN_DIR/_archives" -maxdepth 1 -type f -name 'd1l-release-package-*.zip' -print)
test "${#PACKAGE_ARCHIVES[@]}" -eq 1

PACKAGE_ASSET="$RELEASE_DIR/MeshCore-DeskOS-D1L-v1.0.0-rc.1.zip"
ASSET_SUMS="$RELEASE_DIR/SHA256SUMS.txt"
cp "${PACKAGE_ARCHIVES[0]}" "$PACKAGE_ASSET"
(cd "$RELEASE_DIR" && sha256sum "$(basename "$PACKAGE_ASSET")" > SHA256SUMS.txt && sha256sum --check SHA256SUMS.txt)

git fetch origin main --tags
test "$(git rev-parse origin/main)" = "$SHA"
test "$(jq -r .ready_for_public_release "$AUDIT")" = true
test -z "$(git ls-remote --tags origin refs/tags/v1.0.0-rc.1)"
git tag -a v1.0.0-rc.1 "$SHA" -m "MeshCore DeskOS D1L 1.0.0 RC1"
git push origin refs/tags/v1.0.0-rc.1

RELEASE_ASSETS=(
  "$PACKAGE_ASSET"
  "$ASSET_SUMS"
)

gh release create v1.0.0-rc.1 "${RELEASE_ASSETS[@]}" \
  --repo n30nex/SIGUI \
  --verify-tag \
  --target "$SHA" \
  --title "MeshCore DeskOS D1L 1.0.0 RC1" \
  --prerelease \
  --latest=false \
  --notes "RC1 candidate for actual device use. Extract the package fully and start with START_HERE.md."

test "$(gh release view v1.0.0-rc.1 --repo n30nex/SIGUI --json tagName --jq .tagName)" = v1.0.0-rc.1
test "$(gh release view v1.0.0-rc.1 --repo n30nex/SIGUI --json isDraft --jq .isDraft)" = false
test "$(gh release view v1.0.0-rc.1 --repo n30nex/SIGUI --json isPrerelease --jq .isPrerelease)" = true
```

## 10. Stable v1.0.0 promotion

Stable `v1.0.0` is a byte-identical promotion of the accepted public
`v1.0.0-rc.1` assets. Do not rebuild, repackage, rename, or rerun hardware.
The RC asset filename and internal candidate wording remain unchanged so the
stable release distributes the exact audited bytes. A later documentation-only
commit on `main` does not change the tag target: both tags must peel to `$SHA`.

```bash
RC_TAG=v1.0.0-rc.1
STABLE_TAG=v1.0.0
RELEASE_DIR="$ROOT/artifacts/release/$RC_TAG"
PACKAGE_ASSET="$RELEASE_DIR/MeshCore-DeskOS-D1L-v1.0.0-rc.1.zip"
ASSET_SUMS="$RELEASE_DIR/SHA256SUMS.txt"
EVIDENCE_DIR="$ROOT/artifacts/rc1-final/$SHA"
AUDIT="$EVIDENCE_DIR/rc1-release-audit-${SHA}.json"

test -f "$PACKAGE_ASSET"
test -f "$ASSET_SUMS"
test "$(jq -r .ready_for_public_release "$AUDIT")" = true
test "$(jq -r .identity.firmware_commit "$AUDIT")" = "$SHA"
test "$(jq -r '.failures | length' "$AUDIT")" -eq 0

git fetch origin main --tags
git merge-base --is-ancestor "$SHA" origin/main
test "$(git ls-remote origin "refs/tags/${RC_TAG}^{}" | cut -f1)" = "$SHA"
test "$(gh release view "$RC_TAG" --repo n30nex/SIGUI --json isDraft --jq .isDraft)" = false
test "$(gh release view "$RC_TAG" --repo n30nex/SIGUI --json isPrerelease --jq .isPrerelease)" = true

mapfile -t RC_ASSET_NAMES < <(
  gh release view "$RC_TAG" --repo n30nex/SIGUI --json assets --jq '.assets[].name' | sort
)
test "${#RC_ASSET_NAMES[@]}" -eq 2
test "${RC_ASSET_NAMES[0]}" = MeshCore-DeskOS-D1L-v1.0.0-rc.1.zip
test "${RC_ASSET_NAMES[1]}" = SHA256SUMS.txt

PROMOTION_ROOT="$(mktemp -d)"
RC_PUBLIC_DIR="$PROMOTION_ROOT/rc"
RC_EXTRACT_DIR="$PROMOTION_ROOT/rc-extracted"
STABLE_PUBLIC_DIR="$PROMOTION_ROOT/stable"
STABLE_EXTRACT_DIR="$PROMOTION_ROOT/stable-extracted"
mkdir -p "$RC_PUBLIC_DIR" "$RC_EXTRACT_DIR" "$STABLE_PUBLIC_DIR" "$STABLE_EXTRACT_DIR"

gh release download "$RC_TAG" \
  --repo n30nex/SIGUI \
  --dir "$RC_PUBLIC_DIR" \
  --pattern 'MeshCore-DeskOS-D1L-v1.0.0-rc.1.zip' \
  --pattern 'SHA256SUMS.txt'

RC_PUBLIC_ZIP="$RC_PUBLIC_DIR/MeshCore-DeskOS-D1L-v1.0.0-rc.1.zip"
RC_PUBLIC_SUMS="$RC_PUBLIC_DIR/SHA256SUMS.txt"
test "$(find "$RC_PUBLIC_DIR" -maxdepth 1 -type f | wc -l)" -eq 2
(cd "$RC_PUBLIC_DIR" && sha256sum --check SHA256SUMS.txt)
cmp -- "$PACKAGE_ASSET" "$RC_PUBLIC_ZIP"
cmp -- "$ASSET_SUMS" "$RC_PUBLIC_SUMS"

unzip -q "$RC_PUBLIC_ZIP" -d "$RC_EXTRACT_DIR"
RC_PUBLIC_PACKAGE="$RC_EXTRACT_DIR/d1l-release-${SHA}"
test -f "$RC_PUBLIC_PACKAGE/manifest.json"
"$PY" scripts/verify_checksums.py "$RC_PUBLIC_PACKAGE"
test "$(jq -r .app_version "$RC_PUBLIC_PACKAGE/manifest.json")" = 1.0.0
test "$(jq -r .release_profile "$RC_PUBLIC_PACKAGE/manifest.json")" = core_1_0
test "$(jq -r .firmware_commit "$RC_PUBLIC_PACKAGE/manifest.json")" = "$SHA"

test -z "$(git ls-remote --tags origin "refs/tags/$STABLE_TAG")"
! gh release view "$STABLE_TAG" --repo n30nex/SIGUI >/dev/null 2>&1
git tag -a "$STABLE_TAG" "$SHA" -m "MeshCore DeskOS D1L 1.0.0"
git push origin "refs/tags/$STABLE_TAG"
test "$(git ls-remote origin "refs/tags/${STABLE_TAG}^{}" | cut -f1)" = "$SHA"

STABLE_RELEASE_ASSETS=(
  "$RC_PUBLIC_ZIP"
  "$RC_PUBLIC_SUMS"
)

gh release create "$STABLE_TAG" "${STABLE_RELEASE_ASSETS[@]}" \
  --repo n30nex/SIGUI \
  --verify-tag \
  --target "$SHA" \
  --title "MeshCore DeskOS D1L 1.0.0" \
  --latest \
  --notes "Stable byte-identical promotion of v1.0.0-rc.1. The RC asset filename and internal candidate wording are intentionally retained so these are the exact audited bytes. Extract the package fully and start with START_HERE.md."

test "$(gh release view "$STABLE_TAG" --repo n30nex/SIGUI --json tagName --jq .tagName)" = "$STABLE_TAG"
test "$(gh release view "$STABLE_TAG" --repo n30nex/SIGUI --json isDraft --jq .isDraft)" = false
test "$(gh release view "$STABLE_TAG" --repo n30nex/SIGUI --json isPrerelease --jq .isPrerelease)" = false
test "$(gh api repos/n30nex/SIGUI/releases/latest --jq .tag_name)" = "$STABLE_TAG"

mapfile -t STABLE_ASSET_NAMES < <(
  gh release view "$STABLE_TAG" --repo n30nex/SIGUI --json assets --jq '.assets[].name' | sort
)
test "${#STABLE_ASSET_NAMES[@]}" -eq 2
test "${STABLE_ASSET_NAMES[0]}" = MeshCore-DeskOS-D1L-v1.0.0-rc.1.zip
test "${STABLE_ASSET_NAMES[1]}" = SHA256SUMS.txt

gh release download "$STABLE_TAG" \
  --repo n30nex/SIGUI \
  --dir "$STABLE_PUBLIC_DIR" \
  --pattern 'MeshCore-DeskOS-D1L-v1.0.0-rc.1.zip' \
  --pattern 'SHA256SUMS.txt'

STABLE_PUBLIC_ZIP="$STABLE_PUBLIC_DIR/MeshCore-DeskOS-D1L-v1.0.0-rc.1.zip"
STABLE_PUBLIC_SUMS="$STABLE_PUBLIC_DIR/SHA256SUMS.txt"
test "$(find "$STABLE_PUBLIC_DIR" -maxdepth 1 -type f | wc -l)" -eq 2
(cd "$STABLE_PUBLIC_DIR" && sha256sum --check SHA256SUMS.txt)
cmp -- "$RC_PUBLIC_ZIP" "$STABLE_PUBLIC_ZIP"
cmp -- "$RC_PUBLIC_SUMS" "$STABLE_PUBLIC_SUMS"

unzip -q "$STABLE_PUBLIC_ZIP" -d "$STABLE_EXTRACT_DIR"
STABLE_PUBLIC_PACKAGE="$STABLE_EXTRACT_DIR/d1l-release-${SHA}"
test -f "$STABLE_PUBLIC_PACKAGE/manifest.json"
"$PY" scripts/verify_checksums.py "$STABLE_PUBLIC_PACKAGE"
test "$(jq -r .firmware_commit "$STABLE_PUBLIC_PACKAGE/manifest.json")" = "$SHA"
```

## 11. Failure handling

Any nonzero source, checksum mismatch, candidate mismatch, missing artifact,
wrong stable identity, or audit value other than exact `true` stops publication.
Fix a reproducible code defect in one bounded PR, or record the precise operator
blocker on issue #71. Do not add a soak, regenerate a plan, edit receipts, swap
packages, format SD, or probe another port.
