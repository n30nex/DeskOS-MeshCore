# DeskOS D1L RC1 test plan

## Layer A — change-local tests

Run only the focused source checks mapped to files or behavior changed by the
current patch, plus:

```text
git diff --check
```

Do not run the entire host suite locally by default; simulator images are
useful focused artifacts, but images alone do not pass a behavioral contract.
A required simulator command must exit nonzero when its report says `ok=false`.

## Layer B — one CI qualification

Each PR receives one normal full GitHub Actions qualification. It runs the full
host suite once, checksum/package contracts, MeshCore conformance, required
RP2040 builds, firmware build, and production package generation.

A rerun is allowed only after a patch made for a failure, for a failed or
cancelled infrastructure job, or for a documented flaky external dependency.
Do not rerun a green qualification for confidence.

## Layer C — final exact-candidate hardware pass

Run the four RC1 sources once against the frozen exact `main` candidate: one
non-erasing flash source, one RF source, one protocol source, and one Map
source. Aggregate those sources and run `rc1_release_gate_audit_d1l.py`.

No timed idle, endurance, traffic, listening, or soak gate is part of RC1.

A new test is allowed only when it reproduces an observed defect, fails before
the fix, passes after the fix, protects a user-visible RC1 contract, and is
smaller than the protected code path where reasonably possible.
