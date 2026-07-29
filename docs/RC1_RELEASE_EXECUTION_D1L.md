# DeskOS 1.0 / RC1 Release Execution

This is the authoritative closing procedure for tag `v1.0.0`. It starts only
after the final RC1 change is merged. It uses one exact successful `main`
push, one clean Pi checkout, one downloaded Actions package, one non-erasing
flash, four machine-generated evidence sources, one aggregate, and one
final audit.

Do not add a soak, run another broad test campaign, erase NVS, format SD, use
a local firmware build, or use the package's older `flash_project.sh` helper
as closing evidence. The only Public transmission permitted here is the one
tokenized send enabled by `--authorize-public-tx`.

The completed UI-navigation, saved-profile Wi-Fi reconnect, SD
write/reboot/remount, and prepared-card remove/reinsert gates are not rerun
during closing. The prior operator-observed SD cycle is carried forward as
context only; this procedure does not claim a fresh SD receipt or represent
that cycle as a fresh outcome. The remaining sources still prove the exact
candidate flash, controlled RF/protocol behavior, and authorized Map
download/cache behavior.

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

PY=/home/neonx/sigui-venv/bin/python
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

The capture receipt is valid for at most 24 hours. Complete the single flash
and bounded gate promptly. If it expires, recapture the same exact run from a
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
- a working configured Wi-Fi connection for the retained Map gate (the
  completed Wi-Fi reconnect campaign is not rerun);
- an installed HTTPS Map provider manifest that permits offline storage and
  background prefetch, plus configured device location;
- the pinned local controlled peer and its current status/socket;
- a controlled repeater/admin contact fingerprint and password;

Keep the admin password in a bounded regular file outside the repository. Do
not place credentials in command arguments, evidence, shell history or Git:

```bash
ADMIN_FINGERPRINT=9880BF9B9B1DD605
ADMIN_PASSWORD_FILE=<absolute-path-outside-repository>
test -f "$ADMIN_PASSWORD_FILE"
```

If the controlled admin credentials or authorized provider manifest are
unavailable, stop; do not substitute an unknown peer or use OpenStreetMap
Standard for bulk/offline download.

## 5. Perform one retained-state-preserving flash

Use `core_flash_only_d1l.py`. This is the closing flash path; do not run
`flash_project.sh`. The retained-reflash phase captures the ready compatible
Core baseline already on the D1L, flashes the exact candidate once, and proves
the retained projection survived. The baseline may be a predecessor
`core_1_0` commit; the post-flash version must be the exact release commit.
This phase does not erase flash, erase NVS, format SD, or touch another serial
target.
On Linux, the first admitted stable-identity handle is used only for preflight
and esptool. The raw flash log is persisted, then that esptool-tainted handle
must close before any post-flash reset or console read. The runner revalidates
the stable target and opens one fresh exclusive 115200-baud recovery handle.
That fresh handle performs the explicit EN reset, remains open through at
least 90 seconds of boot settle with no console I/O, and then captures the
post-flash retained state without another reopen. The receipt binds every
target snapshot and proves the reset, settle, and capture used the same fresh
handle, distinct from the closed flash handle. A shorter or non-finite
`--settle-sec` is rejected before flashing.

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

The receipt is evidence source one and must report `closure_eligible=true`,
the ready baseline commit, the exact post-flash candidate commit, preserved
retained state, a passing bound reset/settle contract, and a passing exclusive
readmission capture contract.

## 6. Produce the other three bounded sources

Run these serially against the retained-reflash image in the order shown.
Map acceptance includes one normal product reboot, so it must finish before
the RF and protocol receipts. The protocol receipt remains last because it
contains the sole authorized Public send.

### Source 4: authorized Map download and offline cache revisit

The runner reads the provider authorization already installed on the device.
`--root` binds the transcript to this exact clean source checkout. It opens Map
through the normal product UI, disables Wi-Fi, performs a normal product
reboot, reopens Map from the prepared SD cache with no view-network request,
then restores the original UI tab and saved Wi-Fi connection. It does not
remove, reinsert, erase, or format the SD card.

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

### Source 2: controlled-peer DM/ACK

Run as `neonx`. This exact COM11 identity makes the runner capture the generic
Meshcorebot status itself, send the inbound DM through the pinned local Unix
socket, validate its acknowledged response and retain both request/response
sidecars. `/dev/krab-com11` is an opaque status identity and is never opened.
This RF receipt is the sole authority for outbound/inbound DM, ACK/PATH and
direct-route acceptance; the protocol source below does not repeat that DM
exchange.

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
  --peer-status /opt/canadaverse/com11-meshcorebot/data/logs/meshcorebot.status.json \
  --peer-port /dev/krab-com11 \
  --fingerprint 0BF0A701D5AE2DB6 \
  --d1l-public-key "$D1L_PUBLIC_KEY" \
  --token "rc1-${SHA:0:8}" \
  --timeout 60 \
  --wait-sec 90 \
  --poll-sec 3 \
  --commit "$SHA" \
  --github-run-id "$RUN" \
  --github-run-attempt "$ATTEMPT" \
  --out "$RF"
```

### Source 3: boot advert, Admin/PATH/Ping gates, then one Public send

This is the sole authorized Public send in the closing sequence. Omitting
`--authorize-public-tx` must fail before opening serial. This source covers
Public, contacts, Admin login/query/logout, Admin PATH, Ping and health/crash
only; DM remains exclusively covered by Source 2. Admin login/query/logout,
Admin PATH, and Ping must all complete before the controlled-peer baseline is
captured and the sole Public send is authorized.

Per operator direction, Source 3 does not rerun TRACE. A prior controlled TRACE
was operator-observed on `d26a8cdc2e54a44ebb6c5a182f0e6057d566fb3b`;
no TRACE, routing, radio, contact, or pinned build input changed between that
predecessor and this candidate. That observation is carried forward as release
context only: it is not a fresh Source 3 outcome and is not required by the
final audit.

```bash
PROTOCOL="$EVIDENCE_DIR/protocol-admin.json"
"$PY" scripts/produce_rc1_protocol_acceptance_d1l.py \
  --root "$ROOT" \
  --output "$PROTOCOL" \
  --commit "$SHA" \
  --github-run-id "$RUN" \
  --github-run-attempt "$ATTEMPT" \
  --peer-status /opt/canadaverse/com11-meshcorebot/data/logs/meshcorebot.status.json \
  --peer-control-socket /run/canadaverse-control/com11/control.sock \
  --peer-public-key 0BF0A701D5AE2DB679C641EE999A70D4B55B61A2B77C47337CE35C16C9C19193 \
  --peer-device /dev/krab-com11 \
  --peer-service meshcorebot \
  --peer-status-schema meshcorebot_v1 \
  --admin-fingerprint "$ADMIN_FINGERPRINT" \
  --admin-password-file "$ADMIN_PASSWORD_FILE" \
  --boot-timeout 75 \
  --command-timeout 60 \
  --authorize-public-tx
```

The COM11 chat peer is exact and distinct from the repeater used for Admin:
its status profile pins the service, stable device identity, public key,
status path, control socket and status layout. Public and DM counters must
advance under one stable Meshcorebot process session. `ADMIN_FINGERPRINT`
must still resolve to a separate canonical repeater contact with Admin
capability; never substitute the COM11 chat fingerprint for it.
Before the single Public send, the runner queues one signed D1L flood advert
and requires COM11 to resolve exactly one signed `D1L` contact to the current
D1L key. The subsequent Public receive must reference that exact advert.

## 7. Aggregate the four sources

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

The producer must copy four unique source JSON files into
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

## 9. Stage the production download

The public release contains only the production package ZIP and one outer
checksum manifest. Keep the Actions capture, physical sources, aggregate
receipt, sidecar, and final audit in the internal release workspace; they prove
publication readiness but are not customer downloads. The package audit must
have verified `START_HERE.md`, the Windows/Linux installers, the production
ESP32 and RP2040 firmware, recovery files, licenses, SBOM, provenance, and both
checksum layers.

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

PACKAGE_ASSET="$RELEASE_DIR/MeshCore-DeskOS-D1L-v1.0.0.zip"
ASSET_SUMS="$RELEASE_DIR/SHA256SUMS.txt"

cp "$PACKAGE_ARCHIVE" "$PACKAGE_ASSET"

(
  cd "$RELEASE_DIR"
  sha256sum "$(basename "$PACKAGE_ASSET")" > "$(basename "$ASSET_SUMS")"
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
  "$ASSET_SUMS"
)

gh release create v1.0.0 "${RELEASE_ASSETS[@]}" \
  --repo n30nex/SIGUI \
  --verify-tag \
  --target "$SHA" \
  --title "MeshCore DeskOS D1L 1.0.0" \
  --notes "Production DeskOS D1L 1.0.0 for actual device use. Download MeshCore-DeskOS-D1L-v1.0.0.zip, extract it completely, and start with START_HERE.md for Windows or Linux installation. The download includes production ESP32 and RP2040 firmware, safe recovery, package verification, licenses, SBOM, and provenance."

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
unzip -q "$VERIFY_DIR/MeshCore-DeskOS-D1L-v1.0.0.zip" \
  -d "$VERIFY_DIR/unpacked"
FRESH_PACKAGE="$VERIFY_DIR/unpacked/d1l-release-${SHA}"
"$PY" "$FRESH_PACKAGE/scripts/verify_package.py" "$FRESH_PACKAGE"
```

Release completion means the remote annotated tag resolves to `$SHA`, the
GitHub release is published (not draft or prerelease), every expected asset is
present, the attached `SHA256SUMS.txt` verifies after a fresh download, and the
freshly extracted package passes its complete internal checksum inventory.
