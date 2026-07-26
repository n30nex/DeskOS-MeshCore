# DeskOS 1.0 / RC1 Release Execution

This is the authoritative closing procedure for tag `v1.0.0`. It starts only
after the final RC1 change is merged. It uses one exact successful `main`
push, one clean Pi checkout, one downloaded Actions package, two non-erasing
flashes, eight machine-generated evidence sources, one aggregate, and one
final audit.

Do not add a soak, run another broad test campaign, erase NVS, format SD, use
a local firmware build, or use the package's older `flash_project.sh` helper
as closing evidence. The only Public transmission permitted here is the one
tokenized send enabled by `--authorize-public-tx`.

## 1. Freeze the merged candidate

Use a clean checkout owned and operated by `neonx` on Pi 5 host `neopi5`.
Every evidence runner that stamps source provenance must execute from this
same exact checkout.

```bash
test "$(hostname)" = neopi5
test "$(id -un)" = neonx

ROOT=/home/neonx/SIGUI-rc1
cd "$ROOT"
git fetch origin main --tags

SHA=<40-character-final-merge-commit>
test "$(git rev-parse origin/main)" = "$SHA"
git checkout --detach "$SHA"
test "$(git rev-parse HEAD)" = "$SHA"
test -z "$(git status --porcelain=v1)"
test "$(stat -c %U "$ROOT")" = neonx

PY=/home/siguidev/sigui-venv/bin/python
test -x "$PY"
```

If the checkout is not exact, clean and `neonx`-owned, make a fresh checkout;
do not clean or repurpose another user's working tree.

## 2. Select the exact merged-main Actions run

The qualifying run must be the completed successful `d1l-ci` **push** run on
branch `main` whose `headSha` is exactly `$SHA`. A pull-request run, manual
dispatch, branch run, predecessor run or merely green job is not release
evidence.

```bash
gh run list \
  --repo n30nex/SIGUI \
  --workflow d1l-ci.yml \
  --branch main \
  --event push \
  --commit "$SHA" \
  --status success \
  --limit 1 \
  --json databaseId,attempt,status,conclusion,headSha,headBranch,event,url

RUN=<numeric-run-id-from-that-row>
ATTEMPT="$(gh run view "$RUN" --repo n30nex/SIGUI --json attempt --jq .attempt)"
test "$ATTEMPT" -ge 1
gh run view "$RUN" --repo n30nex/SIGUI --exit-status
```

## 3. Capture exactly eight artifacts

Capture from the clean exact checkout. The capture refuses an existing output
directory and independently requires the successful exact-SHA `main` push and
this exact artifact set:

1. `d1l-host-artifacts`
2. `d1l-meshcore-wire-conformance`
3. `d1l-idf55-migration-state`
4. `d1l-firmware-artifacts`
5. `d1l-release-package`
6. `rp2040-sd-bridge-firmware`
7. `rp2040-sd-smoke-firmware`
8. `rp2040-seeed-official-sd-smoke-firmware`

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
test -f "$PACKAGE/SHA256SUMS.txt"
(cd "$PACKAGE" && sha256sum --check SHA256SUMS.txt)
```

The capture receipt is valid for at most 24 hours. Complete both flashes and
the bounded gate promptly. If it expires, recapture the same exact run from a
fresh exact-clean checkout/evidence path; never edit a timestamp or reuse a
stale receipt.

## 4. Lock the physical inputs

The only D1L target is:

```bash
PORT=/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
test -L "$PORT" && test -r "$PORT" && test -w "$PORT"
D1L_DEVICE_PROPERTIES="$(udevadm info --query=property --name="$PORT")"
grep -qx 'ID_VENDOR_ID=1a86' <<<"$D1L_DEVICE_PROPERTIES"
grep -qx 'ID_MODEL_ID=7523' <<<"$D1L_DEVICE_PROPERTIES"
```

Do not substitute `/dev/ttyUSB*`, a Windows COM assignment, or another Pi
serial device.

Before flashing, confirm the D1L's current 64-hex public key over this exact
route and set it explicitly. Do not infer or regenerate it:

```bash
D1L_PUBLIC_KEY=<confirmed-current-64-hex-D1L-public-key>
test "${#D1L_PUBLIC_KEY}" -eq 64
```

The final gate also requires all of the following before it starts:

- a prepared 32GB-or-larger FAT32 card already mounted by DeskOS;
- the saved Wi-Fi profile `Toddmas2.4` (the Wi-Fi runner accepts no password);
- an installed HTTPS Map provider manifest that permits offline storage and
  background prefetch, plus configured device location;
- the pinned local controlled peer and its current status/socket;
- a controlled repeater/admin contact fingerprint and password.

Keep the admin password in a bounded regular file outside the repository. Do
not place credentials in command arguments, evidence, shell history or Git:

```bash
TARGET_SSID=Toddmas2.4
ADMIN_FINGERPRINT=<16-hex-controlled-admin-fingerprint>
ADMIN_PASSWORD_FILE=<absolute-path-outside-repository>
test -f "$ADMIN_PASSWORD_FILE"
```

If the Wi-Fi profile is absent, stop and request the password ephemerally,
configure it through the device workflow, then resume. If the controlled
admin credentials or authorized provider manifest are unavailable, stop; do
not substitute an unknown peer or use OpenStreetMap Standard for bulk/offline
download.

## 5. Bootstrap, then prove retained reflash

Both phases use `core_flash_only_d1l.py`. This is the closing flash path; do
not run `flash_project.sh`. Neither phase erases flash, erases NVS, formats SD
or touches another serial target.

```bash
EVIDENCE_DIR="$ROOT/artifacts/rc1-final/$SHA"
mkdir -p "$EVIDENCE_DIR"

FLASH_BOOTSTRAP="$EVIDENCE_DIR/flash-bootstrap.json"
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
  --phase bootstrap \
  --out "$FLASH_BOOTSTRAP"

FLASH_RETAINED="$EVIDENCE_DIR/flash-retained-reflash.json"
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
  --phase retained-reflash \
  --out "$FLASH_RETAINED"
```

The retained-reflash receipt, not the bootstrap receipt, is evidence source
one and must report `closure_eligible=true` and preserved retained state.

## 6. Produce the other seven bounded sources

Run these serially against the retained-reflash image. Do not add a soak or
repeat the completed manual display/touch/keyboard/scroll campaign.

### Source 2: automated 12-surface UI navigation

```bash
UI="$EVIDENCE_DIR/ui-navigation.json"
"$PY" scripts/scroll_probe_d1l.py \
  --port "$PORT" \
  --screens home,public_messages,dm_thread,nodes,packets,settings,storage,wifi,map,map_options,map_location,map_cache \
  --release-profile core_1_0 \
  --expected-firmware-commit "$SHA" \
  --github-actions-run "$RUN" \
  --workflow-run-attempt "$ATTEMPT" \
  --expected-sd-history-mode conditional \
  --expected-d1l-public-key "$D1L_PUBLIC_KEY" \
  --out "$UI"
```

### Source 3: controlled-peer DM/ACK

Run as `neonx`; `--peer-local` uses only the pinned local status file, Unix
socket, peer device and peer key.

```bash
RF="$EVIDENCE_DIR/rf-full-acceptance.json"
env \
  -u D1L_DM_TARGET \
  -u MESH_PEER_STATUS_PATH \
  -u MESH_PEER_PORT \
  -u MESH_PEER_SSH_HOST \
  -u MESH_PEER_REMOTE_STATUS_PATH \
  -u MESH_PEER_CONTROL_SOCKET \
  -u MESH_PEER_DEVICE \
  -u MESH_PEER_PUBLIC_KEY \
  "$PY" scripts/rf_full_acceptance_d1l.py \
  --port "$PORT" \
  --peer-local \
  --d1l-public-key "$D1L_PUBLIC_KEY" \
  --commit "$SHA" \
  --github-run-id "$RUN" \
  --github-run-attempt "$ATTEMPT" \
  --out "$RF"
```

### Source 4: boot advert, one Public send, PATH/TRACE/Ping and Admin

This is the sole authorized Public send in the closing sequence. Omitting
`--authorize-public-tx` must fail before opening serial.

```bash
PROTOCOL="$EVIDENCE_DIR/protocol-admin.json"
"$PY" scripts/produce_rc1_protocol_acceptance_d1l.py \
  --root "$ROOT" \
  --output "$PROTOCOL" \
  --commit "$SHA" \
  --github-run-id "$RUN" \
  --github-run-attempt "$ATTEMPT" \
  --peer-status /opt/canadaverse/com15-responder/data/radio_listener.status.json \
  --peer-control-socket /run/canadaverse-control/com15/control.sock \
  --peer-public-key 024999dedfd26763c5606169c3ebd34e05a9475cf78220a81078b5dd27caca44 \
  --admin-fingerprint "$ADMIN_FINGERPRINT" \
  --admin-password-file "$ADMIN_PASSWORD_FILE" \
  --authorize-public-tx
```

### Source 5: one saved-profile Wi-Fi reconnect cycle

```bash
WIFI="$EVIDENCE_DIR/wifi-reconnect.json"
"$PY" scripts/wifi_resilience_d1l.py \
  --port "$PORT" \
  --target-ssid "$TARGET_SSID" \
  --expected-firmware-commit "$SHA" \
  --cycles 1 \
  --out "$WIFI"
```

### Source 6: SD write, reboot and remount

```bash
SD="$EVIDENCE_DIR/sd-reboot-remount.json"
"$PY" scripts/sd_reboot_remount_acceptance_d1l.py \
  --port "$PORT" \
  --expected-firmware-commit "$SHA" \
  --out "$SD"
```

### Source 7: SD degraded notice and reinsert recovery

This runner prompts for one physical remove/reinsert cycle. It is the only
physical intervention in this evidence set. Remove only the prepared DeskOS
card when prompted; do not format or repair it.

```bash
SD_DEGRADED="$EVIDENCE_DIR/sd-remove-reinsert.json"
"$PY" scripts/sd_remove_reinsert_acceptance_d1l.py \
  --port "$PORT" \
  --expected-firmware-commit "$SHA" \
  --strict-evidence \
  --out "$SD_DEGRADED"
```

### Source 8: authorized Map download and offline cache revisit

The runner reads the provider authorization already installed on the device.
`--root` binds the transcript to this exact clean source checkout.

```bash
MAP="$EVIDENCE_DIR/map-acceptance.json"
"$PY" scripts/rc1_map_acceptance_d1l.py \
  --root "$ROOT" \
  --port "$PORT" \
  --expected-firmware-commit "$SHA" \
  --github-actions-run "$RUN" \
  --workflow-run-attempt "$ATTEMPT" \
  --output "$MAP"
```

## 7. Aggregate the eight sources

```bash
PHYSICAL="$EVIDENCE_DIR/rc1-bounded-physical-${SHA}.json"
PHYSICAL_EVIDENCE="$EVIDENCE_DIR/rc1-bounded-physical-${SHA}.evidence.json"

"$PY" scripts/produce_rc1_bounded_physical_receipt_d1l.py \
  --package-dir "$PACKAGE" \
  --flash-receipt "$FLASH_RETAINED" \
  --ui-receipt "$UI" \
  --rf-receipt "$RF" \
  --protocol-receipt "$PROTOCOL" \
  --wifi-receipt "$WIFI" \
  --sd-receipt "$SD" \
  --sd-degraded-receipt "$SD_DEGRADED" \
  --map-receipt "$MAP" \
  --output "$PHYSICAL" \
  --evidence-output "$PHYSICAL_EVIDENCE"
```

The producer must copy eight unique source JSON files into
`rc1-bounded-physical-${SHA}.sources/` and fail on missing, duplicate,
simulated, dry-run, manual-only, stale-source or candidate-mismatched evidence.

## 8. Run the final fail-closed audit

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

Do not tag if the audit exits nonzero or `ready_for_public_release` is not
exactly `true`.

## 9. Stage checksummed release assets

Publish the API-verified release-package archive, its internal package checksum
manifest, the Actions capture, the physical receipt and complete source bundle,
and the final audit. A second checksum manifest binds the uploaded asset files.

```bash
RELEASE_DIR="$ROOT/artifacts/release/v1.0.0"
test ! -e "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"

mapfile -t PACKAGE_ARCHIVES < <(
  find "$RUN_DIR/_archives" -maxdepth 1 -type f \
    -name 'd1l-release-package-*.zip' -print
)
test "${#PACKAGE_ARCHIVES[@]}" -eq 1
PACKAGE_ARCHIVE="${PACKAGE_ARCHIVES[0]}"

PACKAGE_ASSET="$RELEASE_DIR/d1l-release-${SHA}.zip"
PACKAGE_TREE_SUMS="$RELEASE_DIR/d1l-release-${SHA}-package-SHA256SUMS.txt"
CAPTURE_ASSET="$RELEASE_DIR/core-actions-run-${RUN}.json"
PHYSICAL_ASSET="$RELEASE_DIR/rc1-bounded-physical-${SHA}.json"
PHYSICAL_SIDECAR_ASSET="$RELEASE_DIR/rc1-bounded-physical-${SHA}.evidence.json"
PHYSICAL_BUNDLE="$RELEASE_DIR/rc1-bounded-physical-${SHA}.tar.gz"
AUDIT_ASSET="$RELEASE_DIR/rc1-release-audit-${SHA}.json"
ASSET_SUMS="$RELEASE_DIR/SHA256SUMS.txt"

cp "$PACKAGE_ARCHIVE" "$PACKAGE_ASSET"
cp "$PACKAGE/SHA256SUMS.txt" "$PACKAGE_TREE_SUMS"
cp "$CAPTURE" "$CAPTURE_ASSET"
cp "$PHYSICAL" "$PHYSICAL_ASSET"
cp "$PHYSICAL_EVIDENCE" "$PHYSICAL_SIDECAR_ASSET"
cp "$AUDIT" "$AUDIT_ASSET"
tar -C "$EVIDENCE_DIR" -czf "$PHYSICAL_BUNDLE" \
  "$(basename "$PHYSICAL")" \
  "$(basename "$PHYSICAL_EVIDENCE")" \
  "$(basename "${PHYSICAL%.json}.sources")"

(
  cd "$RELEASE_DIR"
  sha256sum \
    "$(basename "$PACKAGE_ASSET")" \
    "$(basename "$PACKAGE_TREE_SUMS")" \
    "$(basename "$CAPTURE_ASSET")" \
    "$(basename "$PHYSICAL_ASSET")" \
    "$(basename "$PHYSICAL_SIDECAR_ASSET")" \
    "$(basename "$PHYSICAL_BUNDLE")" \
    "$(basename "$AUDIT_ASSET")" \
    > "$(basename "$ASSET_SUMS")"
  sha256sum --check "$(basename "$ASSET_SUMS")"
)
```

## 10. Create the exact tag and production release

Reconfirm `origin/main`, the clean source checkout, the final audit and the
absence of an existing tag/release. Then create and push one annotated tag
pointing at the audited SHA and publish the checksummed assets.

```bash
git fetch origin main --tags
test "$(git rev-parse origin/main)" = "$SHA"
test "$(git rev-parse HEAD)" = "$SHA"
test -z "$(git status --porcelain=v1)"
test "$(jq -r .ready_for_public_release "$AUDIT")" = true

if git ls-remote --exit-code --tags origin refs/tags/v1.0.0 >/dev/null 2>&1; then
  echo "v1.0.0 tag already exists; stop"
  exit 1
fi
if gh release view v1.0.0 --repo n30nex/SIGUI >/dev/null 2>&1; then
  echo "v1.0.0 release already exists; stop"
  exit 1
fi

git tag -a v1.0.0 "$SHA" -m "MeshCore DeskOS D1L 1.0.0"
test "$(git rev-list -n 1 v1.0.0)" = "$SHA"
git push origin refs/tags/v1.0.0
test "$(git ls-remote origin 'refs/tags/v1.0.0^{}' | cut -f1)" = "$SHA"

RELEASE_ASSETS=(
  "$PACKAGE_ASSET"
  "$PACKAGE_TREE_SUMS"
  "$CAPTURE_ASSET"
  "$PHYSICAL_ASSET"
  "$PHYSICAL_SIDECAR_ASSET"
  "$PHYSICAL_BUNDLE"
  "$AUDIT_ASSET"
  "$ASSET_SUMS"
)

gh release create v1.0.0 "${RELEASE_ASSETS[@]}" \
  --repo n30nex/SIGUI \
  --verify-tag \
  --target "$SHA" \
  --title "MeshCore DeskOS D1L 1.0.0" \
  --generate-notes \
  --notes "Production Core 1.0 release from exact source ${SHA} and Actions run ${RUN}; see the attached audit and checksummed physical evidence."

test "$(gh release view v1.0.0 --repo n30nex/SIGUI --json tagName --jq .tagName)" = v1.0.0
test "$(gh release view v1.0.0 --repo n30nex/SIGUI --json isDraft --jq .isDraft)" = false
test "$(gh release view v1.0.0 --repo n30nex/SIGUI --json isPrerelease --jq .isPrerelease)" = false
gh release view v1.0.0 --repo n30nex/SIGUI \
  --json url,tagName,targetCommitish,assets

VERIFY_DIR="$(mktemp -d)"
gh release download v1.0.0 \
  --repo n30nex/SIGUI \
  --dir "$VERIFY_DIR"
(cd "$VERIFY_DIR" && sha256sum --check SHA256SUMS.txt)
```

Release completion means the remote annotated tag resolves to `$SHA`, the
GitHub release is published (not draft or prerelease), every expected asset is
present, and the attached `SHA256SUMS.txt` verifies after a fresh download.
