# MeshCore Conformance Boundary

Issue #65 is the fail-closed tracker for MeshCore protocol conformance. The
first CI slice is intentionally limited to the packet wire envelope. Its
machine-readable result must report:

- `coverage_level="wire_envelope_only"`
- `closure_ready=false`

A green result is required before the firmware job may run, but it is not
eligible to close issue #65 or to qualify the firmware for release.

## Pinned Oracle

The structural oracle is the recursively checked-out MeshCore submodule at
`third_party/MeshCore`, currently pinned by the repository gitlink to
`e8d3c53ba1ea863937081cd0caad759b832f3028`. CI compares the original D1L C
wire codec with the packet-envelope implementation at that exact pin. It does
not compare against a moving branch or a separately installed MeshCore copy.

`tests/meshcore_conformance/manifest.json` locks the upstream commit and source
hashes together with the vector and fuzz contract. Because Git may check text
out with CRLF on Windows, tracked text is hashed in its canonical LF form; all
other byte changes still invalidate the pin. The runner must fail closed if the
gitlink, checked-out submodule, source hashes, or manifest disagree.

Any MeshCore gitlink change invalidates the prior result. A pin update requires
an intentional review of the adapter, vector matrix, malformed cases, and this
boundary before new evidence can be accepted.

The upstream native-test AES and SHA mocks remain excluded because they do not
implement production cryptography. The public-group slice instead compiles the
real AES source already vendored with the pinned Seeed BSP and a deterministic
host SHA-256/HMAC adapter, each checked against independent published
known-answer vectors before it is allowed to exercise pinned `mesh::Utils`.

## WP-04 Oracle ABI Foundation

`tests/meshcore_oracle/meshcore_oracle.h` defines version 2 of a narrow C ABI
around the pinned upstream `mesh::Packet` envelope reader/writer and the
`AdvertDataBuilder`/`AdvertDataParser` app-data helpers and bounded
public-group, anonymous-login, regular REQ/RESPONSE, canonical repeater/room
login-response, repeater/room ACL-miss password authorization, blank-password
existing-ACL reuse, authorized-login ACL record transitions, plain-DM, DM
ACK+PATH, authenticated-request and authenticated-TEXT replay/session
transitions, login-response creation/dispatch orchestration, signed-advert
receive dispatch gates, signed-advert creation/send ownership ordering, and
general PATH-return operations. Its version 23 static
`manifest.json` binds that interface, the exact upstream commit, every source
used by the target, and all deterministic vectors by canonical-LF SHA-256. The
packet capability retains four round-trip and five reject vectors. The advert
field capability adds four canonical round trips and eleven invalid-boundary
vectors covering type, location, feature fields, name, truncation, and
reject-without-output-mutation behavior. Seven direct, flood, transport, and
zero-hop preparation round trips plus ten rejects are golden-bound to the
pinned `Mesh.cpp` header/path/priority rules. Non-transport results explicitly
clear ignored transport fields so every accepted C-ABI result has a canonical
wire round trip. Five simple/multipart ACK framing round trips plus seventeen
reject vectors are also golden-bound to `Mesh::createAck` and
`Mesh::createMultiAck`. They cover the payload type, multipart subtype,
remaining-count nibble, the dispatcher's four-byte minimum ACK body, payload
limits, version gate, deterministic wire round trip, and
reject-without-output-mutation behavior. ACK constructors return a canonical
pre-route layout; vectors apply the pinned zero-hop route preparation before
encoding, so that intermediate header is never presented as production wire
fidelity. The `meshcore-conformance` job builds and runs this as a separate
sanitized host target. Six initial outbound TRACE round trips plus nineteen
reject vectors cover the pinned nine-byte tag/auth/flags prefix, flags-zero
contract, zero-to-63 one-byte route hashes appended to the payload, direct
route, zero outer path, priority five, fixed little-endian bytes on the
supported targets, and reject-without-output-mutation behavior. TRACE creation
is likewise pre-route and is never encoded before direct preparation. The job
also compiles the real C Ed25519 verifier vendored at the pinned MeshCore
gitlink. Three fixed production-layout signed-advert vectors cover empty,
named, and maximum-size app data; one independent RFC 8032 empty-message
known-answer vector checks the verifier directly. Eleven signed-advert rejects
cover null or oversized input, changed public key, timestamp, app data or
signature, and a signature whose scalar is increased by the Ed25519 group
order, plus the pinned verifier's identity-public-key/identity-R/zero-S
message-independent forgery. Four accepted point vectors and seven rejected
point vectors cover the fixed-seed and RFC public keys/signature-R values,
null input, non-canonical field encoding, and representative order-one,
order-two, and order-four encodings. The separate verifier KAT accounts for
one valid and three negative or
canonicality-regression cases; it is not counted as an advert semantic vector
or as a round trip. The production receive path and oracle share a strict
`S < L` guard plus canonical-encoding and low-order checks for the advert
public key and signature R before the pinned verifier. These close the
verifier's otherwise malleable `S + L` acceptance and the deterministic
identity-point forgery. Valid vectors run repeatedly under ASan and UBSan.
Three signed-advert packet round trips additionally reproduce the exact
`Mesh::createAdvert` pre-route payload for empty, named, and maximum app data
from the fixed seed and caller-supplied timestamp, apply flood preparation,
wire encode/decode, and authenticate before parsing. Twenty-three rejects cover
constructor/parser nulls, size and capacity limits, type/version/length/path
errors, public-key/timestamp/signature/app-data tampering, low-order points,
non-canonical `S`, and output preservation. This is deterministic packet
creation and authenticated parsing only; RTC/RNG ownership, upstream
`Identity::verify` parity, replay policy, dispatch, contact mutation, duplicate
suppression, and RF remain excluded.
Three crypto-adapter known-answer vectors cover FIPS 180-4 SHA-256 `abc`, RFC
4231 HMAC-SHA-256 test case 1, and the FIPS 197 AES-128 cipher example. Four
public-group round trips and twenty-one reject vectors then cover the
`BaseChatMesh::setChannel` padded-16-secret versus full-32-secret hash rule,
non-empty group text and data types, one- and two-block plaintext, the 168-byte
maximum plaintext, AES-128 ECB zero padding, the two-byte HMAC, deterministic
wire encode/decode, authenticated parsing, and output preservation on every
rejected input. These group vectors are packet creation/parsing proof only;
they do not execute Mesh dispatch, delivery, retained history, or radio paths.
The separate `addChannel` path's explicit decoded key length is not inferred by
this ABI.
The anonymous-login capability adds six authenticated request round trips and
thirty-five rejects. Non-room empty, exact-block 12-byte, and maximum 15-byte
passwords plus room empty, exact-block 8-byte, and maximum 15-byte passwords
pin the little-endian timestamp/optional `sync_since` layout. Two independently
generated exact payloads and a six-payload SHA-256 matrix pin the one-byte
destination hash, 32-byte sender public key, truncated HMAC, AES-128 ECB
ciphertext, pre-route ANON_REQ header, and flood/direct wire round trips.
Rejects cover mandatory inputs and output preservation, embedded NUL and size
limits, room-mode and non-room-sync canonicality, type/version/envelope/length
errors, wrong outer fields or secret, MAC/ciphertext tampering, redundant AES
blocks, nonzero padding, and authenticated overlength passwords. The wire has
no room-mode or logical-length field, so the caller supplies room mode and the
canonical parser returns bytes before the first zero. The anonymous-login
function itself does not derive a shared secret. Contact lookup, authorization,
replay policy, response generation, admin or session mutation, dispatch, route
choice, scheduling, and RF remain excluded.
The regular request/response capability adds six authenticated round trips and
thirty rejects. REQ plaintext lengths 1, 15, 16, and 17 plus RESPONSE lengths
16 and the upstream 167-byte maximum pin both packet types, one-byte
destination/source hashes, AES block boundaries, and the conservative
`Mesh::createDatagram` size limit. Independently generated exact REQ and
RESPONSE payloads, a maximum-payload SHA-256 digest, and a six-payload matrix
digest pin the AES-128 ECB and two-byte HMAC-SHA-256 result. Every accepted
packet receives a deterministic flood or direct wire round trip before an
authenticated parse. Rejects cover unsupported types, nulls, empty/oversized
plaintext, output capacity and preservation, future/mismatched types, envelope
and block lengths, wrong hashes or secret, MAC/ciphertext tampering, invalid
paths, nonzero padding, and redundant blocks. Because the wire carries no
logical plaintext length, parsing requires a caller-supplied schema length and
accepts only its minimal AES block count with zero padding. Empty datagrams are
excluded because the pinned receive path rejects their MAC-only form. Request
tags/types, response schemas beyond the canonical login success schema,
identity or secret derivation, authorization, replay/session/admin state,
dispatch, route choice, and RF remain excluded.
The identity shared-secret capability adds five symmetric valid-input round
trips and nine rejects. Fixed seed pairs, including an all-zero seed, pin the
seed-to-keypair step and the vendored Edwards-y-to-Montgomery ladder. Every
derived public key and 32-byte shared secret is compared with an independently
generated libsodium Ed25519-to-Curve25519 conversion and scalar-multiplication
result; the five secrets also pin an aggregate SHA-256 digest. Both directions
must return the same secret. Rejects cover null inputs/output plus canonical
identity, negative-zero, zero, signed-zero, minus-one, and non-canonical peer
encodings, with no output mutation. The wrapper applies the existing strict
peer-point guard, rejects an all-zero result, and wipes temporary expanded
private-key and secret bytes. These guards are D1L fail-closed policy around
valid-input `LocalIdentity::calcSharedSecret` parity; upstream itself accepts
unchecked peer bytes. Persisted-private-key loading, contact lookup,
authorization, signature handling, session/admin state, dispatch, routing,
local production-call-site hardening, and RF remain excluded.
The canonical login-response capability adds ten authenticated round trips and
thirty-four rejects. Repeater and room guest, read-only, read-write, admin, and
flagged-role permissions pin the shared 13-byte success schema: little-endian
server timestamp, zero response code and legacy keep-alive, server-specific
role indicator, full permissions, four uniqueness bytes, and the pinned
server-specific firmware level. Independently generated repeater-admin and
room-guest exact payloads plus a ten-payload SHA-256 matrix pin the one-byte
destination/source hashes, AES-128 ECB ciphertext, and two-byte HMAC. Every
accepted packet receives a flood or direct wire round trip before parsing.
Rejects cover unsupported server types, nulls, wrong hashes or secret,
type/version/envelope errors, MAC/ciphertext tampering, authenticated schema
and role mismatches, wrong firmware, nonzero padding, legacy `OK`, and output
preservation. This deterministic server-emitted response fixture does not
compare passwords, reuse or mutate an ACL/contact, enforce replay timestamps,
assign a shared secret, transition retained session/push/path state, correlate
the companion pending login, dispatch, route, schedule, or claim RF.
The login-password authorization capability adds sixteen accepted decision
fixtures and seventeen fail-closed rejects. It pins the unmatched-client
branches from the repeater and room examples: admin comparison takes precedence,
repeater guest maps to guest permissions, room guest maps to read-write, and a
room with read-only fallback authorizes any otherwise unmatched password as a
guest. The matrix covers allow, deny, precedence, empty configured passwords,
and the 15-byte wire maximum. Rejects cover unsupported advert/server types,
noncanonical read-only flags, null pointers, overlength or embedded-NUL spans,
a control-byte login prefix that would not enter the upstream password branch,
and output preservation. The fixture deliberately begins after an ACL miss; it
does not prove blank-password existing-ACL reuse, contact lookup or mutation,
identity signatures, shared-secret assignment, replay checks, response creation,
retained session/admin transitions, dispatch, routing, scheduling, or RF.
The blank-password existing-ACL capability adds twelve accepted lookup/reuse
fixtures and fourteen fail-closed rejects. It pins `ClientACL::getClient` to a
first full-32-byte-key match and covers empty, first/middle/last, duplicate,
shared-prefix, all-zero, all-`FF`, full-capacity, and miss cases for repeater and
room routes. On a match, the handler skips password authorization, insertion or
eviction, replay checking, and permission, shared-secret, timestamp, and
activity updates. A flood may invalidate only the client out-path. The
repeater response continues with the caller-derived anonymous secret, while
the room response selects the stored ACL secret. Rejects cover unsupported
server types, a noncanonical flood flag, a nonblank or malformed blank input,
null pointers, over-capacity tables, all output pointers, and output
preservation. This immutable table fixture does not instantiate or mutate
`ClientACL`, load/save contacts, prove insertion/eviction, apply replay/session
transitions, create a response, dispatch, route, schedule, or claim RF.
The authorized-login ACL transition capability adds sixteen accepted state
fixtures and thirteen structural rejects. It models the exact pinned projection
of `ClientACL::putClient` followed by the repeater/room mutation block: full-key
reuse, append, strict least-active non-admin eviction, first-wins activity ties,
and the source's last-slot fallback when a full table has no selectable
non-admin. Equal or older timestamps reject before field updates. A timestamp-
zero new login still leaves the just-appended reset record, matching the source
ordering. Accepted transitions replace only the role bits, shared secret,
sender timestamp, local activity time, room sync/pending/push-failure fields,
and flood out-path length; they also pin repeater guest versus non-guest and
room dirty-contact policy. The matrix covers upper permission-bit preservation,
full-table existing-key reuse, direct/flood routes, and every password-produced
role. Structural rejects cover server/route/role canonicality, null pointers,
capacity, and output preservation. This is an immutable input-to-output
`ClientInfo` projection, not filesystem-backed `ClientACL` execution; full path
bytes, unchanged room scheduling fields, identity signatures, secret derivation,
password comparison, response creation, persistence execution, dispatch,
routing, scheduling, and RF remain excluded.
The authenticated-request replay capability adds fifteen accepted/rejected
state fixtures and twelve structural rejects. It pins the source difference
between repeater and room servers after authenticated request decryption: a
repeater accepts only a strictly newer timestamp and advances timestamp and
activity only after a successful handler result, while a room accepts an equal
timestamp and commits timestamp, activity, and cleared push failures before
handler execution. Direct room keep-alive requests bypass the ordinary handler,
optionally advance `sync_since`, clear `pending_ack`, and are response-eligible
only with a known out path; flooded keep-alives retain the ordinary handler
path. Older requests leave state unchanged, and equality is explicitly marked
as a duplicate even where the room source accepts it. Boundary cases cover
timestamp zero and `UINT32_MAX`, direct/flood routing, handler success/failure,
force-since presence, and unknown out paths. This projection begins after a
validated 5-to-167-byte logical request and does not execute request schemas or
handlers, create response hashes or packets, access storage, derive identity or
secrets, dispatch, route, schedule, or claim RF behavior.
The authenticated-TEXT orchestration capability adds twenty source-valid state
fixtures and seventeen fail-closed structural rejects. It pins the exact
repeater/room order after canonical authenticated text extraction. Repeater
first requires an admin role; room accepts every canonical role. Both accept
equal timestamps as retries and commit timestamp/activity before any handler,
post, ACK, response creation, or dispatch attempt; room additionally clears
push failures. Equality refreshes that session state but never reinvokes a CLI
handler or appends a room post. Repeater plain text is legacy CLI and repeats a
simple ACK on equality; its ACK attempt precedes the non-retry handler. Room
CLI is admin-only and never ACKs; room plain text
from any non-guest role appends a post only when newer and repeats its ACK on
equality, with the post committed before ACK creation. CLI response creation
follows the handler in both servers. Known stored paths select direct dispatch and unknown paths select
flood dispatch independent of the inbound route. A configured room direct ACK
attempts multi-ACK before the mandatory simple ACK. Caller-supplied packet-
creation results prove that session, handler, and retained-post commits are not
rolled back when ACK/response allocation fails. Neither server retains a CLI
response for replay. The projection does not decrypt or parse message bytes,
execute a handler or post store, hash/create packets, operate the packet pool,
execute dispatch/timing/routing, derive identity or secrets, or claim RF.
The login-response creation/dispatch capability adds seventeen source-handled
cases and fourteen fail-closed structural rejects. It uses the pinned encoded
path rules rather than treating `out_path_len` as a byte count: `00`, `3F`,
`40`, `60`, `80`, and `95` are valid one/two/three-byte modes, while `61`,
`96`, and reserved four-byte modes reject without output mutation. Repeater
flood/direct and room flood/direct/unknown-path flows cover PATH-return versus
datagram creation, caller versus stored secret selection, allocation failure,
the room's pre-creation 2000 ms push schedule, and the 300 ms response delay.
Flood is intentionally coarse: mutable receive-region state decides ordinary
versus transport flood, so that scope is reported as required and unproven.
No packet pool, path-byte copy, clock, queue, dispatch, storage, or RF executes.
The signed-advert receive-dispatch capability adds twelve source-handled cases
and nineteen rejects. It pins the complete/self/seen/source-order gates, a
caller-supplied signature result, callback eligibility, and canonical
one/two/three-byte flood path-capacity/forward-policy decisions. Direct packets
with a nonzero path fail closed because `Mesh::onRecvPacket` intercepts them
before the ADVERT switch. D1L rejects app data above 32 bytes while upstream
receive truncates it before verification; that is an explicit strict D1L
divergence, not upstream acceptance parity. The external `Ed25519::verify`
implementation used by `Identity.cpp` remains absent, so
`upstream_identity_parity_proven=false`.
The signed-advert creation/send capability adds sixteen source-handled cases
and nine rejects around the already exact fixed-seed packet vectors. Oversize
data stops before allocation; pool exhaustion sets the full-event condition
before RTC/sign/routing; valid creation reaches the pinned vendored signer.
Invalid flood hash sizes retain caller ownership without route mutation,
invalid direct encoded paths are marked seen then released without queueing,
and valid flood/transport-flood versus direct/zero-hop cases pin route/code,
seen, priority-three/priority-zero, and queue eligibility. Neither upstream nor
D1L self-verifies a newly emitted signature or proves persisted public/private
key consistency. Real allocation, RTC, signing, table/path mutation, queueing,
dispatch, and RF remain outside this projection.
The DM capability adds 268 authenticated round trips: every attempt value from
0 through 255, the normal 160-byte and extended-attempt 158-byte text
boundaries, and all ten normal text lengths from 11 through 155 whose complete
plaintext is an exact AES-block multiple. Exact payload bytes pin attempts 0
and 255; aggregate, maximum, and exact-block payload SHA-256 digests pin the
rest of the matrix. Twenty-nine rejects cover
null inputs, embedded NUL text, both size limits, future/wrong packet types,
truncation, non-block ciphertext, wrong hashes or secret, MAC/ciphertext
tampering, unsupported text flags, missing terminators, malformed extended
attempts, noncanonical padding, overlong authenticated plaintext, and output
preservation. The C ABI takes one-byte source/destination hashes and a
caller-supplied 32-byte shared secret. It does not claim Ed25519 key exchange,
contact lookup, dispatch, ACK behavior, retained state, or delivery.
The expected-ACK capability adds five exact four-byte expected-hash vectors,
including an exact-block normal DM, plus four exact six-byte ACK bodies covering
empty, maximum normal, short extended, and maximum extended DM inputs. Four
authenticated ACK+PATH round trips cover zero-, one-, two-, and three-byte
path-hash encodings, with an exact zero-path payload and an aggregate payload
SHA-256 digest. Thirty-five rejects cover derivation/create/parse nulls, text
limits and embedded NULs, invalid path encodings, missing path data,
future/wrong packet types, length errors, wrong hashes or secret, MAC/ciphertext
tampering, malformed authenticated path bodies, noncanonical padding, and
output preservation. The caller supplies the sender public key, one-byte
source/destination hashes, shared secret, and RNG uniqueness byte. This proves
the bounded expected hash/body and encrypted return-path layout, not Identity,
RNG consumption, route selection, dispatch, ACK correlation, delivery state,
or RF behavior. For a normal DM where `5 + text_len` is exactly divisible by
16, upstream still defines the four-byte expected hash but reads the complete
ACK body's fifth byte beyond initialized decrypted/terminator bytes. The oracle
therefore proves the hash and rejects any deterministic full-body claim for
that boundary.
The general PATH-return capability adds six authenticated round trips and
thirty-five rejects. It covers ordinary extras and the no-extra `0xFF` branch,
with all four RNG uniqueness bytes supplied by the caller, zero/one/two/three-
byte hash encodings, both maximum combined path/extra boundaries, upper-nibble
masking of the received extra type, canonical padding, and output preservation.
Each accepted packet is also routed through one of flood, transport flood,
direct, zero hop, or transport zero hop, pinning transport codes and PATH
priority. An exact zero-path payload and aggregate payload SHA-256 make the
matrix deterministic. This is framing plus caller-selected route preparation;
identity/shared-secret derivation, route selection/storage, contact mutation,
dispatch, reciprocal-path decisions, forwarding, scheduling, and RF remain
outside the boundary. The exact-block DM ACK limitation above is unchanged.
Seed bytes `00` through `1f` regenerate the keypair and all three fixed
signatures on every vector run. The source hashes pin the verifier's transitive
C headers and sources, the keypair/signer/shared-secret recipe, `key_exchange.c`,
`advert_data.h`, the shared
canonical scalar/point guard, the D1L CMake/service binding, pinned
`BaseChatMesh`/`Utils` source, the repeater/room response-schema sources, and the
vendored AES source. The functional host
SHA/HMAC adapter is source-pinned and independently checked; no upstream crypto
mock is accepted. The job emits
`meshcore_oracle_manifest_<full-commit>.json` beside the existing conformance
report.

`tests/meshcore_oracle/coverage_manifest.json` is the machine-readable WP-04
boundary receipt. It accounts for all nine roadmap surfaces, every oracle
capability, the current unresolved capability IDs, and the reviewed
upstream-commit/corpus-version pair. The main oracle manifest pins this policy
file by canonical-LF SHA-256. The conformance runner derives all numeric
`D1L_MESHCORE_PAYLOAD_*` declarations from `main/mesh/meshcore_wire.h` (while
explicitly excluding the `D1L_MESHCORE_PAYLOAD_VER_1` version constant) and
requires exact equality with both the policy registry and the eight-type wire
vector matrix. Consequently a new local payload type, a duplicate code, a
missing roadmap surface, or an unknown capability fails before compilation.
Envelope-vector coverage remains distinct from semantic coverage: the current
eight local types all have envelope vectors, while the policy reports six fully
implemented roadmap surfaces, three partial surfaces, and no wholly blocked surface.
The exact packet registry and blocker receipts are copied into
`meshcore_oracle_manifest_<full-commit>.json`; `wp04_acceptance_ready` and
`closure_ready` remain false.

This foundation is intentionally not the completed WP-04 oracle. Its boundary
is
`pinned_upstream_packet_advert_group_dm_expected_ack_path_return_route_codes_ack_trace_and_signed_advert_creation_strict_verification_and_anonymous_login_request_and_regular_request_response_crypto_and_strict_identity_shared_secret_derivation_and_canonical_login_response_packets_and_login_password_authorization_fixtures_and_existing_acl_blank_login_reuse_fixtures_and_authorized_login_acl_transition_fixtures_and_authenticated_request_replay_transition_fixtures_and_authenticated_text_replay_response_session_orchestration_fixtures_and_login_response_creation_dispatch_orchestration_fixtures_and_signed_advert_dispatch_transition_fixtures_and_signed_advert_creation_send_orchestration_fixtures`.
The `signed_advert_verification` and `signed_advert_packet_creation`
capabilities in the version-23 oracle prove only D1L's bounded
message layout (`public_key || timestamp_wire_bytes || app_data`) and the real
vendored C verifier. Upstream `Identity.cpp` deliberately delegates to a
separate external `Ed25519::verify` implementation whose `Ed25519.h` is absent
from the pinned gitlink. Signed-advert receive/send ordering is projected there.
The receive projection starts only after `filterRecvFloodPacket()` has returned
false; region/filter table behavior is not modeled or claimed. Concrete
replay/timestamp policy, contact/table/path mutation, packet-pool,
queue, and dispatch execution remain inside `Mesh`/`Dispatcher`.

The separate
`tests/meshcore_signed_advert_runtime/meshcore_signed_advert_runtime.cpp` gate
now executes that host semantic slice instead of projecting it. It compiles the
exact pinned `Identity`, `Packet`, `Dispatcher`, `Mesh`, `Utils`,
`AdvertDataHelpers`, `TxtDataHelpers`, and `BaseChatMesh` production sources.
The external verifier is the exact `rweather/Crypto` 0.4.0 PlatformIO registry
archive (version ID 43204, 162696 bytes, SHA-256
`1867740aad0d61bdcbac25f6dbc8eefe6eed9e7b37f48d9d0b9d80500ad431e0`),
with every compiled transitive source independently hash-checked before
extraction. Deterministic radio, clock, RNG, and packet-manager doubles drive
the real create/sign, flood queue, wire serialization, receive filter, external
signature verification, dispatch, `BaseChatMesh` contact promotion, callback,
and raw-advert storage paths; duplicate admission delegates to the exact pinned
upstream `SimpleMeshTables` implementation. The executable asserts a valid
contact, exact/path/transport duplicate suppression, forged-signature rejection,
self suppression, balanced packet ownership, contact lifetime after packet
release, and byte-identical two-run replay. It also compares the production D1L
eight-byte packet hash with upstream for five normal/TRACE framing cases,
including TRACE `uint16_t` little-endian path length and path-byte exclusion. It
does not call an oracle projection helper. The production D1L service applies
the matching 160-entry boot-local cyclic cache only after each RX family's
stronger authority: retained channel/DM admission, exact ACK owner revision,
authenticated PATH replay identity, correlated TRACE result, or verified advert
admission. Cached DM packets still enter bounded durable re-ACK handling;
pending ACK persistence and PATH ACK-only retry remain semantic exceptions to a
generic hash hit.
The receipt also preserves a pinned-upstream quirk instead of hiding it:
`BaseChatMesh::onAdvertRecv` reports `is_new=false` to the discovery callback
for a newly auto-added contact even though the contact count advances from zero
to one. Firmware/UI integration must not treat that callback flag as truthful
until the upstream behavior is reconciled.

This clears only `identity_signed_advert_semantic_runtime`. The broader
`identity_signed_advert` aggregate and `BLK-WP04-IDENTITY-DISPATCH-20260713`
remain open for persisted public/private-key consistency, firmware runtime
binding, retained-contact recovery, exact peer/RF evidence, and final-candidate
hardware proof. The gate explicitly reports those residuals, keeps
`wp04_closure_eligible=false` and `closure_ready=false`, and cannot close WP-04
or issue #65. The oracle also fails closed for a one-byte path at count 63: pinned upstream
wraps the six-bit count to the two-byte/zero-count encoding and drops the path
bytes when serialized. That explicit D1L divergence prevents the foundation
oracle from presenting the upstream overflow boundary as a valid append;
concrete route/path mutation remains pending. The route-header capability likewise
does not claim queue timing, route selection, retransmission, or forwarding;
those remain `route_selection_and_forwarding`. Public/DM dispatch, delivery,
session state, ACK correlation/delivery state, PATH dispatch/route selection,
TRACE forwarding/path discovery, and login request schema/tag handling plus
concrete packet-pool, dispatch, and admin-runtime execution also remain
pending, so
both `wp04_closure_eligible` and `closure_ready` remain false. Expected-ACK
derivation and the ACK-specific encrypted PATH body are now deterministic with
caller-supplied identity/hash/secret/RNG inputs; that bounded primitive must not
be cited as dispatch, correlation, route-selection, or delivery proof. ACK
dispatch, correlation, and delivery state remain a separate
pending capability blocked on deterministic Mesh dispatch, packet-manager,
table, and clock fixtures. General PATH-return extra/no-extra creation,
authenticated parsing, and caller-selected route-code preparation are covered
with caller-supplied hash/secret/RNG inputs. Identity derivation, stored route
selection, reciprocal-path decisions, contact updates, dispatch, forwarding,
scheduling, and RF are still excluded.
TRACE framing does not authenticate `auth_code` or prove discovery; forwarding
still requires identity matching, duplicate tables, radio SNR, scheduling, and
clock fixtures. Those stages require deterministic radio, RNG, RTC,
millisecond-clock, packet manager, mesh-table, contact, and channel fixtures;
they must extend the versioned manifest instead of silently widening what this
artifact claims.

The DM crypto/layout primitive is implemented and the separate valid-input
Ed25519 shared-secret derivation is now pinned, but persisted-key loading and
deterministic identity/contact/session fixtures remain outside its boundary.
Route selection/forwarding requires a
deterministic dispatcher, packet manager, mesh tables, radio, RNG, and clocks.
Anonymous login-request framing is implemented with caller-supplied outer
identity, secret, time, and room mode. Regular REQ/RESPONSE datagram crypto is
also implemented with caller-supplied type, hashes, secret, and logical length,
and deterministic valid-input identity shared-secret derivation is pinned to
the vendored ladder. The canonical repeater/room login-success response schema,
ACL-miss password authorization/denial rules, and blank-password existing-ACL
reuse boundary are implemented. Authorized-login reuse, insertion/eviction,
replay gating, and modeled record-field transitions are also pinned.
Authenticated-request replay and bounded record/session transitions are now
pinned. Authenticated-TEXT replay, session/handler/post ordering, creation
outcomes, and stored-path dispatch selection are also pinned, including the
absence of a retained CLI response. Login-response creation/dispatch ordering,
encoded stored-path modes, exact delay constants, and allocation outcomes are
also pinned, while transport-flood scope remains an explicit region-state
dependency. Request tags/types beyond keep-alive and concrete packet-pool,
dispatch, and admin-runtime execution still require deterministic runtime
fixtures.
These receipts describe missing
oracle prerequisites; they are not evidence that those semantics ran.

## What This Slice Covers

The `meshcore-conformance` Actions job runs on `ubuntu-24.04` with an exact
`clang-18` Debian archive and compiler executable identity recorded in
`.github/d1l-build-inputs.json`. CI checksum-verifies both the downloaded
package and the resolved `clang-18`/`clang++-18` bytes before the gate runs,
then embeds that exact-commit tool-byte receipt in the conformance report. The
runner builds the local wire codec and
the pinned upstream packet-envelope code into a structural vector harness with
AddressSanitizer and the enabled UndefinedBehaviorSanitizer checks. It
separately builds the local
wire decoder as the libFuzzer target, then:

1. exercises valid packet envelopes in both directions;
2. checks route type, payload type/version, transport prefixes, path hash
   widths/counts, and payload boundaries, including the upstream 184-byte
   maximum packet payload;
3. rejects truncated, overlong, reserved, and internally inconsistent local
   receive inputs; and
4. runs 100,000 deterministic local-parser fuzz inputs with seed `13746277`
   (`0xD1C065`).

The valid matrix may carry bytes labelled as text, ACK, advert, group text,
PATH, or multipart packets. In this slice those labels prove only that the
packet-type byte and envelope survive the round trip; they do not prove the
meaning, authentication, or state transition for any packet type.

The pinned `Packet::readFrom` envelope path and
`d1l_meshcore_wire_decode()` intentionally expose all four two-bit header
version values so the structural oracle can compare packet bytes without
pretending to be the Dispatcher. The separate
`d1l_meshcore_wire_decode_v1()` boundary accepts only value zero
(`PAYLOAD_VER_1`) and leaves its output unchanged for malformed or future
versions. The fixed harness and local-parser fuzz target exercise both
properties. The production receive dispatcher applies the v1 semantic boundary
before selecting any crypto or retained-store handler. This closes structural
version/type/length gating only; the artifact must not claim the remaining
Dispatcher semantics are covered.

Release packaging, the reproducibility comparator, and the release audit all
reuse the same completed-report validator. A report with green top-level flags
is rejected unless its pinned source hashes, full vector counts, complete
100,000-run fuzz result, zero findings, sanitizer result, and Clang byte receipt
are all present and consistent with the exact source commit.

The job writes
`artifacts/meshcore-conformance/meshcore_conformance_<full-commit>.json` and
uploads it as `d1l-meshcore-wire-conformance`. Evidence is acceptable only
when it identifies the tested project commit and upstream gitlink, retains the
seed/run/vector/malformed counts plus observed fuzz duration, reports zero
enabled-sanitizer or canary failures, and keeps the two boundary fields above
unchanged. Any libFuzzer crash/timeout input is written beside the JSON under
the uploaded `artifacts/meshcore-conformance` tree rather than discarded with
the temporary build directory.

The same pinned-Clang job independently runs the production USB command parser
gate. `d1l_usb_command_admit_in_place()` receives the actual delimiter-free byte
count from both the normal console and the factory-reset recovery console; it
rejects embedded NUL, invisible C0/DEL bytes, empty canonical commands, and
over-255-byte lines before dispatch, and wipes the full caller-owned buffer on
every rejection. Its native suite executes 100,000 deterministic cases and its
libFuzzer target executes another 100,000 inputs with the hash-bound corpus in
`tests/usb_command_parser/corpus.json`. The exact-commit receipt is uploaded as
`usb_command_parser_fuzz_<full-commit>.json`. This implements only the declared
`usb_protocol_command_parser` fuzz target; it does not close the remaining
WP-05 semantic/fuzz targets, physical USB behavior, hardware, RF, or release.

The pinned-Clang job also runs the production
`d1l_meshcore_packet_semantic_parse()` boundary. The receive dispatcher uses its
explicit raw byte count to select exactly one supported structural family:
Admin RESPONSE, configured-channel TEXT, direct TEXT, simple/multipart ACK,
PATH, TRACE, or signed ADVERT. Truncated frames, future versions, unsupported
types, invalid multipart descriptors, and family-specific length violations
reject with a fully zeroed output view before a handler can mutate state. The
hash-bound 14-seed corpus covers every accepted family plus malformed,
truncated, future-version, invalid-path, unknown-type, multipart-subtype, and
oversized-advert boundaries. Its native suite and ASan/UBSan libFuzzer target
each execute exactly 100,000 cases, and Actions uploads
`meshcore_semantic_packet_fuzz_<full-commit>.json`. This advances only the
declared `semantic_packet_parser` target, making the fuzz summary 4 implemented
/ 1 partial / 3 missing. Decryption, MAC/signature authentication, replay/state
transitions, RF, physical behavior, WP-05 closure, and release remain open.

The pinned upstream Ed25519 ref10 arithmetic sources `fe.c`, `ge.c`, and
`sc.c` contain negative signed left shifts that Clang UBSan correctly reports
as `shift-base` undefined behavior. Production and both host gates now select
the plainly marked SIGUI defined-arithmetic overlay under
`overlays/meshcore_ed25519_defined/` for those three files, while retaining the
unchanged MeshCore gitlink and all other pinned Ed25519 sources. The overlay
uses multiplication by checked-in powers of two, passes RFC 8032 vectors and
256 deterministic baseline/overlay differential cases, and is compiled under
ASan plus the full configured UBSan group without source exceptions. Receipts
must expose an empty exception list and `full_ubsan_clean=true`; any
`-fno-sanitize=` flag is a gate failure. This resolves the arithmetic UB
prerequisite but does not expand the oracle's bounded semantic or hardware
coverage.

## Explicit Non-Coverage

This bounded gate makes no claim about:

- encryption, decryption, MACs, signing/key generation, channel secrets, or
  full identity semantics beyond the bounded exceptions described above. The
  oracle covers public-group, regular REQ/RESPONSE, and caller-secret plain-DM
  AES/HMAC creation and authenticated parsing, expected-ACK derivation,
  ACK-specific and general
  encrypted PATH creation/parsing, caller-selected PATH route-code preparation,
  and the D1L signed-advert message layout and pinned C Ed25519 verifier plus
  valid-input identity shared-secret derivation; it does not cover persisted
  key loading/management, DM sessions,
  request/response schemas or tags beyond the bounded canonical login-success
  response, ACL-miss password decision, blank-password existing-ACL reuse, and
  authorized-login ACL transition fixtures or complete retained-state behavior.
  Production-bound host coverage now includes exact-owner ACK completion, but
  not retained-fault/power-loss or compatible-peer delivery evidence;
- complete semantic dispatch for Public, DM, advert, PATH, trace, or general multipart
  traffic. Public-group payload creation/parsing, plain-DM layout/crypto,
  simple/multipart ACK framing, expected-ACK/ACK-specific PATH layout, general
  PATH-return extra/no-extra layout and route-code preparation, initial
  flags-zero TRACE payload framing, and simple/multipart/ACK+PATH exact-owner
  ACK completion are covered, but broader delivery, PATH dispatch,
  stored route choice/reciprocal decisions, TRACE forwarding/SNR/path discovery,
  correlation, and remaining Dispatcher behavior are not;
- complete official-peer duplicate/replay qualification, persistent generic
  duplicate state, or non-advert self-message behavior. Production now binds
  Public/channel, DM, simple/multipart ACK, authenticated PATH, correlated TRACE,
  and advert RX to the boot-local cache without replacing their stronger
  retained/correlation authorities. The separate signed-advert runtime gate covers the
  exact upstream/D1L packet hash, real-table exact/path/transport duplicate
  suppression, self suppression, packet lifetime, and contact lifetime;
- complete persisted/retained state, schema migration, reboot recovery,
  write-fault, or power-loss durability beyond the bounded ACK CAS/reconcile
  transition;
- radio behavior, interoperability with an official client or second radio,
  or any hardware path; or
- the complete declared MeshCore 1.0 surface or release readiness.

Accordingly, a passing artifact must still say `closure_ready=false`. Full
issue #65 closure requires later semantic, production-cryptography, all-type
dispatch/lifetime, persistent/retained-state, and real-peer coverage against
the entire declared surface. Those later stages must not reinterpret this
artifact as evidence they ran.

## CI Reproduction Contract

The authoritative run is GitHub Actions, not a local firmware build:

```bash
python ./scripts/meshcore_conformance_d1l.py \
  --cc clang-18 \
  --cxx clang++-18 \
  --commit "$GITHUB_SHA" \
  --seed 13746277 \
  --runs 100000 \
  --out "artifacts/meshcore-conformance/meshcore_conformance_${GITHUB_SHA}.json"

python ./scripts/meshcore_signed_advert_runtime_d1l.py \
  --cc clang-18 \
  --cxx clang++-18 \
  --commit "$GITHUB_SHA" \
  --sanitize \
  --out "artifacts/meshcore-conformance/meshcore_signed_advert_runtime_${GITHUB_SHA}.json"
```

The firmware job depends on this job succeeding. That dependency prevents a
known structural regression from producing a release package; it does not turn
the structural check into a full protocol or production release gate.

The package step copies the exact current-commit JSON and records its size,
SHA-256, source commit, generation/expiry time, boundary, and non-closure flags.
The release audit also requires an exact clean package source tree, an explicit
selected Actions run ID, and that run's exact-commit host-check success marker.
Mismatched, cross-run, red-host-check, dirty, tampered, path-escaping,
out-of-range, expired, or closure-claiming structural evidence fails closed.
