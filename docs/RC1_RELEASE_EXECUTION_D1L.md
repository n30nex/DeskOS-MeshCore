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
bounded Public send. The D1L Public send is terminal: every local serial,
health, crashlog, identity, and inbound-retention check completes first, and
only the controlled peer receipt is observed afterward.

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

## 10. Stable byte-for-byte promotion

Stable promotion is a separate, explicit maintainer action after the RC1 audit
and prerelease verification above. It does not rebuild or repackage firmware.
The stable ZIP must be byte-identical to the audited RC1 ZIP, and both tags must
peel to the exact accepted commit.

```bash
set -euo pipefail

test "$(jq -r .ready_for_public_release "$AUDIT")" = true
git fetch origin main --tags
test "$(git rev-parse origin/main)" = "$SHA"
test "$(git rev-parse 'v1.0.0-rc.1^{commit}')" = "$SHA"
test "$(gh release view v1.0.0-rc.1 --repo n30nex/SIGUI --json isDraft --jq .isDraft)" = false
test "$(gh release view v1.0.0-rc.1 --repo n30nex/SIGUI --json isPrerelease --jq .isPrerelease)" = true
test -z "$(git ls-remote --tags origin refs/tags/v1.0.0)"
if gh release view v1.0.0 --repo n30nex/SIGUI >/dev/null 2>&1; then
  echo "Refusing to replace an existing v1.0.0 release" >&2
  exit 1
fi

STABLE_DIR="$ROOT/artifacts/release/v1.0.0"
test ! -e "$STABLE_DIR"
mkdir -p "$STABLE_DIR"
STABLE_ASSET="$STABLE_DIR/MeshCore-DeskOS-D1L-v1.0.0.zip"
STABLE_SUMS="$STABLE_DIR/SHA256SUMS.txt"
cp -- "$PACKAGE_ASSET" "$STABLE_ASSET"
cmp --silent "$PACKAGE_ASSET" "$STABLE_ASSET"
(cd "$STABLE_DIR" && sha256sum "$(basename "$STABLE_ASSET")" > SHA256SUMS.txt && sha256sum --check SHA256SUMS.txt)

git tag -a v1.0.0 "$SHA" -m "MeshCore DeskOS D1L 1.0.0"
git push origin refs/tags/v1.0.0

gh release create v1.0.0 "$STABLE_ASSET" "$STABLE_SUMS" \
  --repo n30nex/SIGUI \
  --verify-tag \
  --target "$SHA" \
  --title "MeshCore DeskOS D1L 1.0.0" \
  --latest \
  --notes "Stable DeskOS 1.0 for SenseCAP Indicator D1L. Extract the package fully and start with START_HERE.md."

test "$(git rev-parse 'v1.0.0^{commit}')" = "$SHA"
test "$(gh release view v1.0.0 --repo n30nex/SIGUI --json tagName --jq .tagName)" = v1.0.0
test "$(gh release view v1.0.0 --repo n30nex/SIGUI --json isDraft --jq .isDraft)" = false
test "$(gh release view v1.0.0 --repo n30nex/SIGUI --json isPrerelease --jq .isPrerelease)" = false

VERIFY_DIR="$(mktemp -d "$ROOT/artifacts/release/v1.0.0-verify.XXXXXX")"
gh release download v1.0.0 \
  --repo n30nex/SIGUI \
  --dir "$VERIFY_DIR" \
  --pattern 'MeshCore-DeskOS-D1L-v1.0.0.zip' \
  --pattern 'SHA256SUMS.txt'
(cd "$VERIFY_DIR" && sha256sum --check SHA256SUMS.txt)
cmp --silent "$PACKAGE_ASSET" "$VERIFY_DIR/MeshCore-DeskOS-D1L-v1.0.0.zip"
```

## 11. Failure handling

Any nonzero source, checksum mismatch, candidate mismatch, missing artifact,
wrong stable identity, or audit value other than exact `true` stops publication.
Fix a reproducible code defect in one bounded PR, or record the precise operator
blocker on issue #71. Do not add a soak, regenerate a plan, edit receipts, swap
packages, format SD, or probe another port.
