# DeskOS 1.0 RC1 release checklist

This checklist is a human-readable mirror of
`scripts/rc1_release_gate_audit_d1l.py`. The script is the sole release
decision; checking this page does not declare readiness.

- [ ] `package_checksum_tree_and_manifest` — the package checksum tree and manifest are valid.
- [ ] `package_core_1_0_conditional` — the package is exactly `core_1_0` with `conditional` SD history.
- [ ] `package_sd_primary_truth_and_preparation` — SD is primary when ready, degraded operation is visible without media, and retained history remains without silent default-NVS fallback.
- [ ] `package_exact_commit_run_attempt` — package commit, Actions run, and attempt match the candidate.
- [ ] `actions_successful_main_push_exact_eight_artifacts_and_package` — the successful exact-SHA `main` push contains exactly the required eight artifacts and package.
- [ ] `package_stable_pi_install_contract` — install instructions admit only the stable D1L by-id identity and expected VID/PID.
- [ ] `package_production_only_public_surface` — customer files contain only the production surface.
- [ ] `package_exact_app_artifact` — the application artifact matches the package identity and checksum.
- [ ] `one_bounded_physical_receipt` — one bounded physical receipt binds the exact candidate.
- [ ] `physical_evidence_sidecar_machine_sources` — the sidecar contains four unique machine-generated sources.
- [ ] `receipt_exact_package_binding` — physical receipt and evidence bind the audited package exactly.
- [ ] `stable_pi_path_and_vid_pid` — every physical source uses the stable path and expected VID/PID.
- [ ] `non_erasing_exact_app_flash` — the exact app was flashed without erase or SD format.
- [ ] `formats_sd_false_and_settings_preserved` — every source reports `formats_sd=false` and the flash proves settings were preserved.
- [ ] `bounded_gate_without_soak_or_duration_requirement` — the aggregate is bounded and introduces no soak or duration gate.
- [ ] `boot_advert_and_one_public_send` — the protocol source proves boot advert and exactly one authorized Public send.
- [ ] `dm_ack` — the RF source proves controlled bidirectional DM and truthful ACK.
- [ ] `path_and_ping` — the required PATH and Ping behavior passed.
- [ ] `repeater_login_and_query` — authorized repeater login and query passed.
- [ ] `authorized_map_download_and_cache_revisit` — an authorized fresh Map download and SD cache revisit passed.

No soak is required for RC1.

Do not publish or promote the candidate unless the exact-candidate RC1 audit
reports `ready_for_public_release=true`.
