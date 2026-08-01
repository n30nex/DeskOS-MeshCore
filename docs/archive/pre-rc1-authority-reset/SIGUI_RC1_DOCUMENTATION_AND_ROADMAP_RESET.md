HISTORICAL RECORD — DO NOT EXECUTE

This document predates the RC1 authority reset. It is retained only for
provenance. It cannot create work, tests, evidence requirements, or release
gates. See `docs/RC1_SCOPE.md` and `docs/ROADMAP.md`.

# SIGUI RC1 Documentation and Roadmap Authority Reset

**Repository:** `n30nex/SIGUI`  
**Audit snapshot:** `main` at `def77ac671b55af4089c4ed022ad4a806488cef1`  
**Observed successful Actions run:** `30524554432`  
**Prepared:** 2026-08-01  
**Purpose:** This is the **first document to execute**. Do not begin another firmware feature campaign, broad audit, test-expansion project, or “completion roadmap” until this authority reset is merged.

---

## 1. Executive directive

SIGUI does not presently suffer from a lack of plans, tests, evidence formats, or agent activity. It suffers from **too many mutually conflicting authorities**.

The repository currently contains:

- a production `core_1_0` build profile with a deliberately bounded feature surface;
- a current RC1 release audit designed around one exact package and four bounded physical evidence sources;
- old “full feature completion” roadmaps that continue to demand RC2 work, broad refactors, old COM-port workflows, large evidence banks, and soak testing;
- a root Codex bootstrap prompt that still tells agents to obey those superseded plans;
- a stale completion ledger that can pass its validator while describing an older commit;
- a legacy 36-gate release audit that reports 35 failures in normal CI because it is run without final hardware inputs, even though it is not the current RC1 release authority;
- many old `P0` issues and Codex branches whose wording no longer matches the current build profile or implementation.

This creates the loop:

1. An agent reads the stale bootstrap and historical roadmap.
2. It treats historical work packages and issues as current RC1 requirements.
3. It adds another test, runner, evidence schema, status document, or narrow patch.
4. The current product still lacks one clean, exact-candidate closing pass.
5. A later agent starts over, sees the same red historical ledger, and repeats the cycle.

The first release task is therefore **not another feature implementation**. It is a small, controlled **authority-reset pull request** that makes one path authoritative and makes every superseded path unmistakably historical.

After the reset:

> RC1 work exists only when it maps to a failed check in the current RC1 contract or to a reproducible defect in the exact candidate.

No percentage, old work package, stale issue label, predecessor receipt, speculative hardening idea, or broad architectural preference may create RC1 work by itself.

---

## 2. Current factual baseline

This reset is based on the following observed repository state.

### 2.1 The active build is already a bounded RC1 product

`main/app/release_profile.c` defines `core_1_0` as a deliberate product profile.

The profile currently enables:

- board, display, touch, backlight, Home, and core navigation;
- Public messaging and direct messaging;
- contacts, nodes, packet inspection, routes/signal, and user trace;
- radio settings and identity;
- conditional SD history;
- diagnostics and truthful time;
- Map and user-controlled Wi-Fi;
- multi-channel management;
- authenticated administration;
- observer/MQTT;
- terminal functionality;
- configured location.

It deliberately disables:

- BLE companion transport;
- signed update/recovery;
- advanced QR/emoji surface;
- the development USB recovery service.

That is the starting scope. RC1 must not be silently expanded to make every development or future capability “complete.”

### 2.2 Current automated build health is strong

The observed exact checkout at `def77ac671b55af4089c4ed022ad4a806488cef1` completed:

- host checks;
- MeshCore conformance work;
- ESP32 firmware build;
- RP2040 SD bridge build;
- release packaging.

The host suite reported:

```text
2510 passed, 7 skipped
```

The focused checksum/package set reported:

```text
35 passed
```

A green build does not prove the physical product, but it does prove that the project is not at the beginning of a 25-work-package implementation program.

### 2.3 The active RC1 gate is already bounded correctly

`scripts/rc1_release_gate_audit_d1l.py` is the correct shape for RC1. It binds:

- one exact repository commit;
- one exact successful GitHub Actions run and attempt;
- one exact production package;
- one exact non-erasing flash;
- four machine-readable physical source areas:
  - flash/target truth;
  - RF/DM acceptance;
  - protocol acceptance;
  - Map acceptance;
- no SD formatting;
- retained settings;
- no timed soak or duration requirement.

Its final user-level outcomes are bounded to the release surface:

- boot advert;
- exactly one authorized Public send;
- DM ACK;
- PATH and Ping;
- repeater login and query;
- authorized Map download and cache revisit.

This script, its package contract, and its final output are the release authority.

### 2.4 The current process has false authorities

The following are not acceptable as current RC1 authorities:

- `CODEX_BOOTSTRAP_PROMPT.md` in its current form;
- `docs/completion/SIGUI_CODEX_5_6_ULTRA_GOAL_PROMPT.md`;
- `docs/completion/SIGUI_MASTER_COMPLETION_ROADMAP_2026-07-12.md`;
- `docs/completion/SIGUI_EXECUTION_BACKLOG_2026-07-12.yaml`;
- `docs/COMPLETION_LEDGER.yaml`;
- `docs/COMPLETION_STATUS.md`;
- `scripts/release_gate_audit_d1l.py` when interpreted as an RC1 audit;
- historical checkpoint sections appended to active documentation;
- old issue descriptions treated as executable specifications.

These files may remain for provenance, but they must not be discoverable as the active plan.

---

## 3. Specific root causes that must be removed

### 3.1 The root bootstrap points directly into the obsolete program

The current `CODEX_BOOTSTRAP_PROMPT.md` tells the lead agent to read the superseded completion prompt, master completion roadmap, YAML backlog, and evidence index, then continue until a “Full Feature Completion” release.

That instruction directly conflicts with the current `core_1_0` profile and RC1 contract. It reactivates RC2 work every time a new Codex thread starts.

**Required correction:** Replace the root bootstrap completely. Do not append a warning while leaving the old commands below it.

### 3.2 The committed completion ledger is stale but validates as green

The current `docs/COMPLETION_LEDGER.yaml` still describes `main` at an older commit, while the observed checkout is `def77ac…`.

`scripts/completion_ledger.py` validates the ledger’s internal consistency, but it does not fail merely because the ledger’s `repository.main.commit` differs from the checked-out `HEAD`. The current CI therefore reported the completion-ledger validation as passing while the ledger described an obsolete project state.

This is not a trustworthy release-control system.

**Required correction:** Remove the completion ledger from active CI and active planning. Do not replace it with another committed file that claims to contain the exact current commit. A file containing its own final commit SHA creates a self-reference problem and will become stale again.

Use the current GitHub context, package manifest, receipts, and RC1 audit output to generate exact-candidate status as **CI artifacts**, not as a manually maintained committed ledger.

### 3.3 The legacy full-release audit creates meaningless red noise

The current workflow runs `scripts/release_gate_audit_d1l.py` as a dry-run. The observed run reported 35 failed checks out of 36 because final hardware directories, exact run inputs, long historical matrices, and physical receipts were not present.

That same script still contains broad historical requirements such as a 12-hour soak and old WP-01 evidence concepts. The workflow remains green, so the result is neither a real gate nor useful feedback. It is a permanent red dashboard that invites agents to “fix” non-RC1 requirements.

**Required correction:** Remove this audit from the active RC1 workflow and all active RC1 documentation. Preserve it only as a historical/full-feature or RC2 diagnostic if there is a concrete future use.

### 3.4 Current documents contain a valid current section followed by pages of superseded history

Several documents begin with the modern bounded policy and then append the old program beneath it. Agents do not reliably respect a sentence saying “everything below is historical” when hundreds of lines below still contain priorities, percentages, blockers, test commands, and `P0` language.

This affects, at minimum:

- `docs/ROADMAP.md`;
- `docs/RELEASE_CHECKLIST.md`;
- `docs/TEST_PLAN_D1L.md`;
- `docs/KNOWN_LIMITATIONS.md`;
- `docs/FAST_RELEASE_WORKFLOW_D1L.md`;
- `docs/release/SIGUI_CORE_1_0_PRODUCT_CONTRACT_2026-07-18.md`;
- the completion directory.

**Required correction:** Active documents must contain only active instructions. Historical text must be moved or isolated, not merely placed under a warning halfway down the same file.

### 3.5 Open issues are being used as specifications instead of records

Several open issues describe broad implementation programs that conflict with current source and package claims. Examples include the ESP-IDF migration, Wi-Fi safety, Messages redesign, Nodes redesign, contact/channel lifecycle, Map rendering, route/trace, administration, and UI modularization.

An old issue can still contain useful acceptance language, but its `P0` label is not proof that the current candidate lacks the behavior.

**Required correction:** Every RC1 issue must be reconciled against the current source, current package, and the current RC1 audit. An issue may be:

- closed as implemented;
- closed as superseded;
- folded into the single RC1 release controller;
- moved to RC2/debt;
- narrowed to one reproducible defect;
- narrowed to one missing final evidence source.

No issue remains RC1-active merely because an earlier agent called it `P0`.

### 3.6 Branch debris makes old work look live

The repository has well over 100 `codex/*` branches, many of which correspond to already merged, superseded, or abandoned slices. A fresh agent can mistake these branches for active parallel work and spend tokens comparing or resurrecting them.

**Required correction:** Delete merged and superseded remote branches using a deterministic ancestry/PR-state rule. Preserve only branches with an open PR or a written maintainer exception.

### 3.7 Release naming is ambiguous

The package and README use production `1.0.0` language while the project is still being closed as “1.0 RC1.” A release candidate and a stable release are not the same state.

**Required correction:** Use `v1.0.0-rc.1` for the first publicly releasable candidate. Reserve `v1.0.0` for the later stable promotion. Reconcile any already-created `v1.0.0` release before publishing another candidate.

### 3.8 RC1 CI is exercising the wrong simulator profile

The observed `d1l-ci` run invoked `tools/ui_simulator.py` without an explicit release profile. Its generated reports identify themselves as `release_profile: full_feature`, even though the firmware/package candidate is `core_1_0`.

That allows BLE, advanced QR, development-only, and other deferred surfaces to influence the current RC1 signal.

**Required correction:** Every required RC1 simulator invocation must pass the explicit `core_1_0` profile. A full-feature simulator run may remain informational for RC2, but it must be named and reported separately.

### 3.9 A failed simulator report is currently allowed to look green

The observed `storage-states` simulator report returned:

```json
"ok": false
```

because its required-label expectation did not match the rendered storage/onboarding state. The GitHub Actions step still completed successfully.

**Required correction:** Reconcile whether the expectation or rendered copy is wrong. Then make required checks exit nonzero when their report is not okay. If a scenario is intentionally informational, remove it from required RC1 qualification and label it informational.

### 3.10 The current customer package contains documentation drift

The exact candidate package has at least two concrete documentation defects:

1. `docs/USER_GUIDE_D1L.md` links to `ADMIN_REMOTE_CLI_ALLOWLIST.md`, but that file is not included in the package.
2. `README_RELEASE.md` says retained Core data uses NVS when SD history is disabled, while the same production package declares `storage_authority=sd_primary_live_only_without_sd` and its user guide says retained history is not redirected into default NVS.

**Required correction:** Fix the source docs/package inventory and add one package-link/authority consistency test. Do not add another documentation framework.

---

## 4. New authority model

After the reset, authority must be resolved in this order.

### Tier 1 — compiled product truth

1. `main/app/release_profile.c`
2. build configuration and package manifest
3. exact built binary metadata

These establish what the RC1 image actually includes.

### Tier 2 — static RC1 contract

1. `docs/RC1_SCOPE.md`
2. `docs/RC1_RELEASE_EXECUTION_D1L.md`
3. `docs/TEST_PLAN_D1L.md`

These define the intentionally supported product and the bounded closing process.

### Tier 3 — machine release decision

1. `scripts/rc1_release_gate_audit_d1l.py`
2. its tested package/receipt contracts
3. its exact output for the candidate

The sole final predicate is:

```json
"ready_for_public_release": true
```

from the RC1 audit, bound to the exact package and exact candidate.

### Tier 4 — active human work queue

1. `docs/ROADMAP.md`
2. the RC1 milestone/controller issue

The roadmap may explain the next action, but it cannot add requirements beyond Tiers 1–3.

### Tier 5 — reference documentation

User guide, build decision, protocol notes, attribution, architecture descriptions, and troubleshooting references are useful but do not independently create release gates.

### Historical material

Anything under an archive/completion-history location is evidence or history only. Historical files may explain why a decision was made. They cannot direct current work.

---

## 5. Canonical active document set

The authority-reset PR must leave a small, obvious active set.

| File | Role | Rule |
|---|---|---|
| `AGENTS.md` | Always-on repository instructions for lead and sub-agents | Short, strict, references only canonical files |
| `CODEX_BOOTSTRAP_PROMPT.md` | Human-pasteable start instruction | Execution only; no obsolete roadmap references |
| `README.md` | Public product/status page | Truthful RC1-candidate wording; no premature stable-release claim |
| `docs/README.md` | Documentation index | Separates active, reference, RC2, and archive |
| `docs/RC1_SCOPE.md` | Immutable RC1 include/defer contract | Mirrors `core_1_0`; changes require maintainer approval |
| `docs/ROADMAP.md` | Small current execution board | Only D0 and R1–R6; no percentages or 25-package graph |
| `docs/TEST_PLAN_D1L.md` | Test-budget and evidence policy | Focused local tests, one CI pass, one bounded final hardware pass |
| `docs/RC1_RELEASE_EXECUTION_D1L.md` | Exact closing runbook | One candidate, one package, four evidence sources, one audit |
| `docs/RELEASE_CHECKLIST.md` | Human mirror of the RC1 audit | Must not contain additional gates |
| `docs/KNOWN_LIMITATIONS.md` | Current user-visible limitations only | No checkpoint ledger or obsolete status |
| `docs/RC2_BACKLOG.md` | Deferred 1.5/RC2 and technical debt | BLE, OTA, QR, broad refactors, optional enhancements |

There must not be a second active goal prompt, execution backlog, completion ledger, release checklist, or roadmap.

---

## 6. Required first pull request: `docs/rc1-authority-reset`

### 6.1 Scope freeze for this PR

This PR may change:

- documentation;
- agent instructions;
- issue/milestone metadata;
- CI invocations that select the correct authority;
- minimal tests that ensure documentation authority remains coherent.

It must not:

- add a firmware feature;
- refactor runtime code;
- alter MeshCore wire behavior;
- change storage schemas;
- change radio behavior;
- add another evidence framework;
- run hardware;
- create a release tag.

A firmware defect discovered during the reset is recorded in the new roadmap and handled only after the reset merges.

### 6.2 Branch and PR rules

Suggested branch:

```text
codex/rc1-authority-reset
```

Suggested PR title:

```text
docs(release): reset SIGUI RC1 authority and retire legacy completion loop
```

One lead agent owns this PR. Read-only sub-agents may inspect independent groups of files, but only the lead edits shared authority files.

---

## 7. File-by-file change instructions

### 7.1 Add `AGENTS.md`

Create a short repository-level instruction file containing these non-negotiable rules:

- RC1 means `core_1_0`; do not expand it.
- Read only the canonical active files first.
- Historical completion files are not executable.
- The current RC1 audit is the final release authority.
- Local testing is focused by changed slice.
- GitHub Actions builds firmware.
- No timed soak is required.
- Never format SD.
- Never probe arbitrary ports.
- Use only the stable authorized D1L identity for final hardware work.
- Do not create a new roadmap, ledger, evidence schema, dashboard, or test framework unless an observed release defect cannot be handled by the existing path.
- At most one implementation PR is active.
- A failed check must produce a code fix or a clear operator blocker; it must not produce another planning document.

Keep this file operational. Do not paste the entire handoff into it.

### 7.2 Replace `CODEX_BOOTSTRAP_PROMPT.md`

Delete the existing content and replace it with a concise pointer to:

1. `AGENTS.md`
2. `docs/RC1_SCOPE.md`
3. `docs/ROADMAP.md`
4. `docs/TEST_PLAN_D1L.md`
5. `docs/RC1_RELEASE_EXECUTION_D1L.md`
6. `SIGUI_RC1_IMPLEMENTATION_HANDOFF.md` if that handoff is committed

It must explicitly say:

- this is execution, not a new audit;
- the historical completion prompt, master roadmap, backlog, ledger, and legacy release audit are not current authority;
- the first task is the highest unblocked row of `docs/ROADMAP.md`;
- do not return only another plan.

### 7.3 Rewrite `README.md`

The public README must use one truthful status sentence.

Before the RC1 audit passes:

```text
Current status: DeskOS 1.0 RC1 candidate under final exact-package acceptance.
No stable v1.0.0 release is claimed until the RC1 audit reports
ready_for_public_release=true.
```

After the RC1 audit passes and the prerelease is published:

```text
Current status: DeskOS v1.0.0-rc.1 release candidate.
```

The README must:

- describe the `core_1_0` feature surface;
- list deferred features;
- link the user guide and release candidate;
- avoid completion percentages;
- avoid historical commit/run ledgers;
- avoid making the maintainer’s private LAN IP part of public installation instructions;
- distinguish ordinary user installation from maintainer final-candidate acceptance;
- reserve stable `v1.0.0` wording until stable promotion.

### 7.4 Rewrite `docs/README.md`

Use four sections only:

1. **Active RC1 authority**
2. **User/reference documentation**
3. **RC2/deferred work**
4. **Historical archive**

Every active file appears once. Every historical planning file appears only in the archive section with a warning.

### 7.5 Create `docs/RC1_SCOPE.md`

This document must mirror the compiled `core_1_0` product contract.

Use a table with three states:

- Included and release-gated
- Included but degraded/conditional
- Deferred from RC1

Do not use “partial,” “advanced,” or percentages.

At minimum, defer:

- BLE companion transport;
- signed OTA/update/recovery;
- advanced QR/contact/channel sharing;
- broad UI architecture refactoring;
- telemetry expansion beyond current release diagnostics;
- feature additions discovered during testing.

State that a change to this file requires explicit maintainer approval and a matching change to the release profile/package contract.

### 7.6 Replace `docs/ROADMAP.md`

The active roadmap must be short enough to read in one screen or a few screens.

Use exactly this work model:

| ID | Work | Completion predicate |
|---|---|---|
| D0 | Authority reset | This PR merged; legacy authorities inactive |
| R1 | Freeze exact main package | Successful `push` run on `main`; artifacts downloaded and checksums verified |
| R2 | Exact flash source | One non-erasing exact app flash; correct by-id/VID/PID; settings retained; no SD format |
| R3 | RF source | Controlled bidirectional DM with truthful ACK and required route behavior |
| R4 | Protocol source | Boot advert, one Public send, PATH, Ping, repeater login/query |
| R5 | Map source | Authorized fresh tile download plus cache revisit from SD |
| R6 | Aggregate and release | Four sources match final candidate; RC1 audit true; prerelease assets published |

The roadmap may show state and current blocker, but it must not contain:

- completion percentages;
- more than these seven rows;
- long historical evidence narratives;
- old WPs;
- a future-feature backlog;
- speculative defects;
- a soak phase.

### 7.7 Replace `docs/TEST_PLAN_D1L.md`

The active test plan must define three layers.

#### Layer A — change-local tests

Run only tests mapped to files/behavior changed by the current patch, plus:

```text
git diff --check
```

Do not run the entire host suite locally by default.

#### Layer B — one CI qualification

Each PR receives one normal full CI qualification. A rerun is allowed only for:

- a failed or cancelled infrastructure job;
- a patch made after a failure;
- a documented flaky external dependency.

A green run is not rerun for confidence.

#### Layer C — final exact-candidate hardware pass

Run the four RC1 sources once on the frozen final candidate.

No timed idle, endurance, traffic, listening, or soak gate is part of RC1.

A new test is allowed only when it:

- reproduces an observed defect;
- fails before the fix;
- passes after the fix;
- protects a user-visible RC1 contract;
- is smaller than the code path it protects where reasonably possible.

### 7.8 Shorten `docs/RC1_RELEASE_EXECUTION_D1L.md`

Retain the useful exact-package and four-source contract, but remove repeated explanations and historical narrative.

The runbook should contain:

1. Preconditions
2. Candidate identity capture
3. Artifact download/checksum verification
4. Stable target admission
5. Non-erasing flash
6. Four source commands
7. Evidence aggregation
8. RC1 audit command
9. Prerelease publication
10. Failure handling

Use environment variables for operator-specific values. Keep the stable by-id identity check, but do not use a private IP as public product documentation.

### 7.9 Replace `docs/RELEASE_CHECKLIST.md`

This checklist is a human-readable mirror of `scripts/rc1_release_gate_audit_d1l.py`.

Every checkbox must map to a named machine check. If a checkbox has no corresponding audit check, either:

- add a justified machine check to the authoritative audit, with tests and maintainer approval; or
- remove the checkbox.

The checklist must end with:

```text
Do not publish or promote the candidate unless the exact-candidate RC1 audit
reports ready_for_public_release=true.
```

It must not reintroduce a second release decision.

### 7.10 Replace `docs/KNOWN_LIMITATIONS.md`

Keep only current user-visible facts, including:

- no onboard GPS;
- configured location behavior;
- conditional SD and degraded live-only behavior;
- BLE deferred;
- signed OTA/update/recovery deferred;
- QR sharing deferred;
- provider policy for Map;
- any verified current functional limit.

Move all old checkpoint percentages, PR histories, run IDs, evidence bank descriptions, and obsolete blockers out of this file.

### 7.11 Create `docs/RC2_BACKLOG.md`

This is the only future-work backlog.

Suggested headings:

- BLE companion completion, based on the existing draft PR only after rebase/review;
- signed update, rollback, and recovery;
- advanced QR/contact/channel sharing;
- UI module/debt work that is not required to repair an observed RC1 defect;
- telemetry/diagnostic expansion;
- optional UX enhancements;
- longer reliability qualification after RC1;
- technical-debt issues moved from RC1.

No RC2 item is allowed to appear as an RC1 gate.

### 7.12 Archive the obsolete completion system

At minimum, remove these from the active path:

```text
docs/completion/SIGUI_CODEX_5_6_ULTRA_GOAL_PROMPT.md
docs/completion/SIGUI_MASTER_COMPLETION_ROADMAP_2026-07-12.md
docs/completion/SIGUI_EXECUTION_BACKLOG_2026-07-12.yaml
docs/completion/SIGUI_AUDIT_EVIDENCE_INDEX_2026-07-12.md
docs/COMPLETION_LEDGER.yaml
docs/COMPLETION_STATUS.md
docs/FAST_RELEASE_WORKFLOW_D1L.md
docs/release/SIGUI_CORE_1_0_PRODUCT_CONTRACT_2026-07-18.md
```

Preferred approach:

- move planning text to `docs/archive/pre-rc1-authority-reset/`;
- preserve evidence files without rewriting or regenerating them;
- add `docs/archive/pre-rc1-authority-reset/README.md`;
- put this banner at the top of archived planning files:

```text
HISTORICAL RECORD — DO NOT EXECUTE

This document predates the RC1 authority reset. It is retained only for
provenance. It cannot create work, tests, evidence requirements, or release
gates. See docs/RC1_SCOPE.md and docs/ROADMAP.md.
```

Avoid a giant rename-only PR for thousands of evidence files. It is acceptable to leave the evidence directory physically in place if:

- its index labels it historical;
- no active file links to it as execution authority;
- the bootstrap ignores it;
- CI does not validate it as current progress.

### 7.13 Retire the old completion-ledger validator from RC1 CI

Remove:

```text
python ./scripts/completion_ledger.py validate --check-generated
```

from the active `d1l-ci` RC1 path.

Options, in preferred order:

1. Keep `scripts/completion_ledger.py` only as a historical utility and add a clear deprecation header.
2. Move it to an archive/tooling location if that does not create a large collateral diff.
3. Delete it only after confirming no provenance workflow still needs it.

Do not spend an RC1 week refactoring this script.

### 7.14 Retire the legacy release audit from RC1 CI

Remove the current dry-run invocation of:

```text
scripts/release_gate_audit_d1l.py
```

from RC1 status generation.

If retained, rename its CI output and documentation to make scope explicit, for example:

```text
legacy-full-feature-audit
```

It must not appear in:

- the active README status;
- the active roadmap;
- the RC1 release checklist;
- the agent bootstrap;
- a branch-protection requirement for RC1.

Do not modify its dozens of old gates to make them “green.” That would waste time and preserve the wrong model.

### 7.15 Make the RC1 audit the sole release predicate

Keep and test:

```text
scripts/rc1_release_gate_audit_d1l.py
```

The final release process must invoke it with explicit:

- package directory;
- Actions capture receipt;
- physical receipt;
- physical evidence aggregate;
- output path.

No other script may claim `ready_for_public_release`.

### 7.16 Correct silent CI results

The observed CI generated a `storage-states` simulator result with `ok: false` while the job still completed successfully.

The reset PR must choose one of two legitimate treatments:

- **Required:** the command exits nonzero and blocks CI when `ok` is false.
- **Informational:** remove it from required checks and stop presenting it as a passing qualification.

Do not keep a command that emits a failed result while the workflow calls the step green.

Apply the same rule to every check: required checks block; informational artifacts are clearly named and do not create work.

### 7.17 Reduce routine CI noise

Do not rewrite all test runners in this PR. Make only the smallest selection change needed.

The active normal PR/main workflow should retain:

- full host suite once;
- checksum/package contract tests;
- MeshCore conformance;
- firmware build;
- RP2040 build when required by changed paths or the final candidate;
- production package generation.

Do not run a full hardware-evidence matrix in dry-run mode merely to generate hundreds of files on every PR. Dry-run contract behavior should live in unit tests or one small contract command.

---

## 8. No committed “exact current SHA” ledger

Do not introduce `RC1_STATUS.json` as another manually edited exact-SHA ledger.

The exact candidate already has machine truth in:

- `GITHUB_SHA`;
- `GITHUB_RUN_ID`;
- `GITHUB_RUN_ATTEMPT`;
- package manifest;
- checksum manifests;
- Actions capture receipt;
- physical source receipts;
- final RC1 audit output.

Generate candidate status under the Actions artifact tree, for example:

```text
artifacts/rc1/rc1-release-gate-audit-<sha>.json
```

A committed roadmap may say which row is active. It must not pretend to be the cryptographic/exact evidence authority.

This prevents the same stale-ledger problem from being recreated under a new filename.

---

## 9. Minimal documentation-authority guard

Add one small automated guard, either as a Python test or existing repository test extension.

It should verify only stable authority rules:

- `CODEX_BOOTSTRAP_PROMPT.md` references the canonical active documents;
- it does not reference the old master completion roadmap/backlog as instructions;
- active documents do not link archived planning files as current authority;
- `docs/ROADMAP.md` contains only `D0`, `R1`–`R6`;
- active roadmap/checklist contain no completion percentages;
- active test plan says no timed soak for RC1;
- workflow does not invoke the old completion ledger as an RC1 gate;
- workflow does not invoke the legacy full-feature audit as an RC1 gate;
- final release documentation names `rc1_release_gate_audit_d1l.py`;
- archived planning files carry a historical warning.

Do not build a new documentation framework, schema engine, or graph validator. A direct test is sufficient.

---

## 10. Issue and milestone reset

### 10.1 Create one RC1 milestone and one controller

Use one milestone:

```text
DeskOS 1.0 RC1
```

Use issue `#71` as the release controller unless a cleaner existing controller is already preferred.

The controller should contain only:

- R1 package;
- R2 flash;
- R3 RF;
- R4 protocol;
- R5 Map;
- R6 aggregate/release;
- links to observed defects opened during those steps.

### 10.2 Reconcile open issues by evidence, not by title

Perform a read-only source/package/evidence reconciliation first. Do not run new hardware tests merely to decide issue classification.

#### Likely controller/final-evidence issues

Review these for folding into `#71` or narrowing to one final source:

```text
#7   RF/DM acceptance
#8   current physical screenshots/final bounded evidence
#11  guided SD install/final operator path
#12  Map prefetch/cache final acceptance
#14  Map renderer final acceptance
#66  DM ACK/delivery final acceptance
#68  PATH/trace final acceptance
#73  Map node-marker final acceptance
#77  repeater/room administration final acceptance
```

Do not leave nine independent RC1 epics if their only remaining requirement is represented by R2–R5.

#### Likely source-implemented or superseded issues

Review the current source, package surface, and merged history, then close or fold these rather than implementing the old issue text wholesale:

```text
#13  Wi-Fi memory/boot safety
#63  ESP-IDF 5.5.4 migration
#67  contacts and multi-channel
#69  persistence/time recovery
#70  role truth/non-forwarding behavior
#74  Messages redesign
#75  Nodes redesign
#76  compact navigation/Home status
```

A remaining concrete defect may be extracted into a small defect issue. The broad original issue does not remain the execution specification.

#### Likely RC2/debt issues

Move these out of RC1 unless a current reproducible crash/data-loss/security defect requires a narrow patch:

```text
#6   broad UI modularization
#17  test architecture expansion
#18  telemetry/heartbeat expansion
#19  optional Finder/Ping/Trace porting or polish
#20  QR/contact sharing and paging polish
#21  OTA/update workflow
#22  prefs/schema discipline expansion
#23  broad public-doc structure polish
```

The open BLE draft PR `#199` belongs to RC2. Do not rebase, merge, or finish it during RC1 closure.

### 10.3 Labels

Use labels that describe present work rather than historical urgency:

```text
release:rc1
release:rc2
kind:defect
kind:evidence
kind:debt
state:blocked-operator
state:superseded
```

A `P0` or release-blocker label requires:

- a reproducible current-candidate failure;
- a user-visible RC1 consequence;
- a bounded completion predicate;
- an owner.

### 10.4 Defect issue template

Every new RC1 defect issue must contain:

```text
Candidate commit/run:
Observed behavior:
Expected RC1 behavior:
Reproduction:
Relevant logs/receipt:
Smallest suspected owner:
Focused test:
Completion predicate:
Affected final source: R2 | R3 | R4 | R5
```

Do not open issues for speculative improvements discovered during code reading. Put them in RC2 backlog.

---

## 11. Pull-request and branch reset

### 11.1 Active work limits

After the reset:

- one active implementation PR at a time;
- one optional read-only investigation thread;
- no stacked feature PRs;
- no agent may create a new branch until the current owner records why the active branch cannot proceed;
- merge or close each PR before starting the next release defect.

### 11.2 Remote branch cleanup rule

Delete a `codex/*` branch only when all are true:

- it is not `main`;
- it has no open PR;
- its PR is merged/closed or its commits are already reachable from `main`;
- it is not explicitly preserved for RC2;
- it is not the authority-reset branch.

Use Git/PR state, not branch-name guesses.

Illustrative local checks:

```bash
git fetch --all --prune
git branch -r --merged origin/main
```

Then confirm associated PR state before deleting remotely.

Do not make branch deletion the critical path to the docs-reset merge. Clean obvious merged branches in a bounded batch and document any exceptions.

### 11.3 PR `#199`

Label/milestone it as RC2 and leave it untouched during RC1. Its own description already identifies it as a BLE foundation rather than release closure.

---

## 12. Version and GitHub release reset

### 12.1 Correct candidate version

Use:

```text
v1.0.0-rc.1
```

for the first public release candidate.

Use:

```text
v1.0.0
```

only for stable promotion after RC1 use and maintainer approval.

### 12.2 Reconcile any existing `v1.0.0`

Before changing tags, inspect:

```bash
gh release view v1.0.0
git show-ref --tags
```

If an unaudited `v1.0.0` exists:

- do not move or delete the tag automatically;
- record its exact target and assets;
- remove any misleading public “stable/latest” claim through the safest GitHub release-state change available;
- publish the correctly named RC only after the final RC1 audit;
- move/delete/recreate a public tag only with explicit maintainer approval and a written note, because tag rewriting damages provenance.

### 12.3 Package wording before closure

Before the final audit passes, package/document headings should say:

```text
DeskOS 1.0 RC1 candidate
```

not:

```text
final stable production 1.0
```

The compiled semantic version may remain the intended 1.0 line, but public release status must be truthful.

---

## 13. Agent operating rules after the reset

### 13.1 Ponytail operating cycle

Ponytail is assumed to be installed already. It is not a project to audit or modify.

Use Ponytail as a **minimal implementation and complexity-control layer**, never as a replacement for correctness, security, exact evidence, Actions-only firmware builds, dirty-worktree protection, or physical qualification.

For every SIGUI coding task, begin with:

```text
$ponytail full
```

Use it to find the smallest root-cause fix and leave one focused runnable check.

After implementation and focused tests pass, run **Ponytail Review** on the resulting diff. Review reports removable complexity but applies nothing. Apply only findings approved by the maintainer, return to `$ponytail full` to implement those selected changes, and rerun the focused tests.

Run **Ponytail Debt** only when the code contains explicit deferred markers such as:

```c
// ponytail: global lock; switch to per-session locks if contention appears
```

Debt identifies the deferred item and the trigger that would justify upgrading it. Do not turn untriggered debt into RC1 work.

Run **Ponytail Audit** separately and periodically, at a milestone or before approved architectural work. It is not part of every task, every PR, or the final RC1 defect loop. Accepted audit findings become separate, small tasks in the appropriate milestone; they do not silently expand the active RC1 patch.

Required task cycle:

```text
Task
  -> $ponytail full implementation
  -> focused tests
  -> Ponytail Review
  -> approved fixes only
  -> focused tests / one CI qualification
  -> Ponytail Debt only if markers exist
  -> PR
```

Pasteable per-task instruction:

```text
Use $ponytail full to implement this SIGUI task. Preserve all safety,
release-evidence, compatibility, and hardware-validation requirements.
After tests pass, run Ponytail Review on the resulting diff and report only.
Apply only the findings I approve. Then run Ponytail Debt if any
ponytail: markers were added.

Review and audit detect over-engineering; they do not replace correctness,
security, hardware, or release-gate review. For SIGUI, exact evidence,
Actions-only firmware builds, dirty-worktree protection, and physical
qualification must never be simplified away.
```

### 13.2 Sub-agent limit

Use at most three bounded roles:

1. **Investigator** — read-only, identifies the exact failing path.
2. **Implementer** — one owner for the affected files.
3. **Reviewer/release verifier** — checks diff, tests, and evidence contract.

The lead remains responsible for integration and cannot delegate the project into an unbounded swarm.

### 13.3 No-plan loop

An agent may update the active roadmap state. It may not respond to an implementation task by producing:

- another master roadmap;
- another audit;
- another completion percentage;
- another YAML work graph;
- another evidence index;
- another test matrix;
- another dashboard;
- another “future team prompt.”

### 13.4 Two-attempt rule

After two patches fail to repair the same observed behavior:

1. stop changing code;
2. inspect exact logs and source ownership;
3. write a short root-cause statement;
4. choose one new targeted hypothesis;
5. apply one patch.

Do not add tests around symptoms indefinitely.

---

## 14. Definition of done for the authority-reset PR

The PR is complete only when all conditions below are true.

### Documentation

- [ ] `AGENTS.md` exists and points only to the canonical RC1 path.
- [ ] `CODEX_BOOTSTRAP_PROMPT.md` no longer activates the superseded full-feature program.
- [ ] `README.md` uses truthful RC1-candidate status.
- [ ] `docs/README.md` clearly separates active, reference, RC2, and archive.
- [ ] `docs/RC1_SCOPE.md` mirrors `core_1_0`.
- [ ] `docs/ROADMAP.md` contains only D0 and R1–R6.
- [ ] `docs/TEST_PLAN_D1L.md` defines focused local tests, one CI pass, and one bounded final hardware pass.
- [ ] `docs/RC1_RELEASE_EXECUTION_D1L.md` defines one package and four evidence sources.
- [ ] `docs/RELEASE_CHECKLIST.md` mirrors the RC1 audit.
- [ ] `docs/KNOWN_LIMITATIONS.md` contains current limitations only.
- [ ] `docs/RC2_BACKLOG.md` contains deferred work.
- [ ] Historical planning files are isolated and marked non-executable.

### CI and scripts

- [ ] Active RC1 CI no longer validates the stale completion ledger.
- [ ] Active RC1 CI no longer presents the legacy full-feature audit as RC1 status.
- [ ] `rc1_release_gate_audit_d1l.py` is the only script allowed to declare public release readiness.
- [ ] Any required simulator/check command fails the step when its own result is `ok: false`.
- [ ] Informational checks are unmistakably informational.
- [ ] A small authority guard prevents the old bootstrap/roadmap references from returning.
- [ ] Existing host, conformance, firmware, RP2040, and package builds remain green.

### GitHub hygiene

- [ ] One RC1 milestone/controller exists.
- [ ] Broad stale P0 issues are closed, folded, narrowed, or moved to RC2.
- [ ] PR `#199` is explicitly RC2.
- [ ] Obvious merged Codex branches are pruned in one bounded batch or recorded for later cleanup.
- [ ] There is no second active roadmap or goal prompt.

### Scope integrity

- [ ] No product feature was added.
- [ ] No runtime refactor was performed.
- [ ] No hardware was run.
- [ ] No release was tagged.
- [ ] No new completion framework was introduced.

---

## 15. Required completion report from the agent

When the authority-reset PR is ready, the lead agent reports only:

```text
Authority-reset PR:
Files replaced:
Files archived:
CI authorities removed:
Canonical RC1 authority:
Issues closed/folded/moved:
Branches pruned:
Tests run:
CI result:
Remaining R1 blocker:
```

Do not provide another prose audit unless a concrete contradiction could not be resolved.

---

## 16. Stop condition for this document

This document has been successfully executed when:

1. the authority-reset PR is merged to `main`;
2. a new agent starting from the repository root cannot accidentally discover the superseded 25-work-package/full-feature program as current instructions;
3. the active roadmap contains only D0 and R1–R6;
4. normal CI no longer emits a meaningless legacy 35-failure RC1 status;
5. the next task is unambiguously **R1: freeze one exact successful main-push package**.

At that point, stop documentation restructuring and proceed to the separate RC1 implementation handoff.

---

## Appendix A — authority decision in one sentence

> **For DeskOS 1.0 RC1, compiled `core_1_0` scope plus `scripts/rc1_release_gate_audit_d1l.py` is authoritative; everything else either supports that path or is historical/RC2.**

## Appendix B — prohibited RC1 scope expansion

Unless the maintainer explicitly changes `docs/RC1_SCOPE.md`, do not use RC1 time or tokens for:

- BLE completion;
- signed OTA/update/recovery;
- advanced QR/contact sharing;
- broad UI-module refactoring;
- general telemetry expansion;
- new architecture layers;
- new release dashboards;
- new evidence schemas where current receipts suffice;
- full-repository fuzz expansion;
- timed soak/endurance testing;
- SD electrical characterization or multi-card qualification beyond the supported final test card;
- cleanup unrelated to the exact observed release blocker;
- rewriting working code for style.

## Appendix C — why this reset is legitimate, not a shortcut

This reset does not waive safety or conceal missing evidence.

It keeps:

- exact Actions provenance;
- checksum verification;
- supported ESP-IDF build;
- host tests;
- MeshCore conformance;
- exact target admission;
- non-erasing flash;
- retained-state truth;
- controlled RF/DM proof;
- protocol proof;
- authorized Map/SD proof;
- fail-closed final audit;
- no-format safety.

It removes duplicated planning, obsolete requirements, permanent red artifacts, and repeated tests that do not change the release decision. That is how SIGUI reaches RC1 faster without pretending an unproven behavior works.
