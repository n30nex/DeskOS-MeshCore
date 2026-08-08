# DeskOS D1L developer checks

This file is for development and CI only. It is not a public release
requirement, is not included in the production package, and must not turn an
end-user firmware release into a lab campaign.

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

No controlled peer, admin password, timed soak, endurance loop, fuzz rerun, or
evidence aggregate is a 1.0 publication requirement. The attached D1L is used
once through the documented normal install and ordinary product UI.

A new developer check is allowed only when it reproduces an observed defect,
fails before the fix, passes after the fix, protects a user-visible 1.0
contract, and is smaller than the protected code path where reasonably
possible.
