# Codex Goal Prompt For Iterative D1L Release Work

> **SUPERSEDED AGENT PROMPT — DO NOT COPY OR EXECUTE.**
> Its COM routing, validation scope and release finish line predate the current
> RC1 contract. Start from the [project README](../README.md),
> [RC1 user guide](USER_GUIDE_D1L.md), [RC1 test plan](TEST_PLAN_D1L.md), and
> [SD-card guide](D1L_SD_CARD_GUIDED_INSTALL.md) instead.

Copy this into the next Codex goal when you want another fast release-blocker
cycle.

```text
Read the active repo docs first, especially:
- README.md
- docs/ROADMAP.md
- docs/RELEASE_CHECKLIST.md
- docs/KNOWN_LIMITATIONS.md
- docs/TEST_PLAN_D1L.md
- docs/FAST_RELEASE_WORKFLOW_D1L.md

Objective:
1. Start from current main.
2. Pick exactly one open P0 GitHub issue that moves MeshCore DeskOS D1L toward public release.
3. Use sub-agents only for bounded sidecar review/testing; I am the lead and must keep the critical path moving locally.
4. Fix the selected issue with the smallest safe code/docs/test change.
5. Run focused host tests, then the full host test suite.
6. Create or update a PR for the fix.
7. Build firmware with GitHub Actions using the fastest correct path:
   - Default ESP32/UI path: `include_sd_bridge=false`.
   - Do not build, flash, test, or validate RP2040/COM16 unless the selected issue changes SD/RP2040 code or SD evidence is the issue.
   - The SD bridge is considered working unless this issue breaks it.
8. Download Actions artifacts and verify checksums.
9. Validate only the hardware surface required by the issue:
   - ESP32/UI default: COM12 only, using the narrow proof command that matches the selected issue.
   - Use `--skip-sd-suite --include-ui-probes` only when the selected issue explicitly spans multiple UI gates or when running a final production sweep.
   - Do not touch COM8, COM11, COM16, or COM29 unless the issue explicitly requires that route and the route is safe.
10. Update the GitHub issue and PR with evidence paths.
11. Merge the PR if checks and targeted hardware proof pass.
12. Close the issue if its acceptance criteria are met.
13. Dismiss all sub-agents.
14. Report exactly what changed, tests run, Actions run, hardware artifacts, what remains blocked, and the next best P0.

Hard constraints:
- COM12 is the D1L ESP32 app/console side.
- COM16 is the D1L RP2040/UF2 side and must not be used for ESP32/UI issues.
- Do not touch COM8, COM11, or COM29 during D1L ESP32/UI validation.
- Do not format SD cards. Keep `formats_sd=false`.
- Do not run 500-cycle tab-abuse tests. Do not run every UI probe for a one-surface issue. Use targeted UI corruption, pixel capture, compose capture, scroll probe, RF/DM proof, SD proof, or the issue-specific validator.
- Prefer short issue-sized PRs over broad release sweeps.
- If release remains blocked after the issue closes, move to the next P0 rather than expanding scope.
```
