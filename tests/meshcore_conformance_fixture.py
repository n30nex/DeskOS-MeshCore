from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

from scripts import meshcore_conformance_d1l as conformance
from scripts import meshcore_signed_advert_runtime_d1l as signed_runtime
from scripts import verify_ci_tool_inputs


def semantic_dependency_receipt(matrix: dict, cc: str) -> dict:
    admin = {
        "main/mesh/meshcore_admin_dispatch.c",
        "main/mesh/meshcore_admin_dispatch.h",
        "tests/native/meshcore_admin_dispatch_test.c",
    }
    runtime = {
        "main/mesh/meshcore_command_guard.h",
        "main/mesh/meshcore_runtime_guard.h",
        "tests/native/meshcore_runtime_guard_test.c",
    }
    trace = {
        "main/mesh/meshcore_trace.h",
        "main/mesh/meshcore_wire.c",
        "main/mesh/meshcore_wire.h",
        "tests/native/meshcore_trace_test.c",
    }
    migration = {
        "main/app/settings_envelope.c",
        "main/app/settings_envelope.h",
        "main/app/settings_protocol_migration.c",
        "main/app/settings_protocol_migration.h",
        "main/mesh/store_lock.h",
        "main/platform/time_service_core.h",
        "tests/native/esp_nvs_stubs.c",
        "tests/native/mock_esp_nvs.h",
        "tests/native/settings_protocol_migration_test.c",
        "tests/native/stubs/esp_err.h",
        "tests/native/stubs/esp_timer.h",
        "tests/native/stubs/freertos/FreeRTOS.h",
        "tests/native/stubs/freertos/semphr.h",
        "tests/native/stubs/nvs.h",
    }
    text_admission = {
        "main/mesh/meshcore_text_plaintext.c",
        "main/mesh/meshcore_text_plaintext.h",
        "main/mesh/user_text.c",
        "main/mesh/user_text.h",
        "tests/native/meshcore_text_plaintext_test.c",
    }
    ack_delivery = {
        "main/mesh/dm_delivery_state.c",
        "main/mesh/dm_delivery_state.h",
        "main/mesh/meshcore_ack_completion.h",
        "main/mesh/meshcore_dm_retry.h",
        "main/mesh/meshcore_wire.h",
        "tests/native/meshcore_ack_completion_test.c",
    }
    advert_admission = {
        "main/mesh/contact_store.c",
        "main/mesh/contact_store.h",
        "main/mesh/contact_uri.c",
        "main/mesh/contact_uri.h",
        "main/mesh/meshcore_advert_admission.c",
        "main/mesh/meshcore_advert_admission.h",
        "main/mesh/meshcore_packet_hash.c",
        "main/mesh/meshcore_packet_hash.h",
        "main/mesh/meshcore_path_state.c",
        "main/mesh/meshcore_path_state.h",
        "main/mesh/meshcore_wire.c",
        "main/mesh/meshcore_wire.h",
        "main/mesh/node_store.c",
        "main/mesh/node_store.h",
        "main/mesh/store_lock.h",
        "tests/native/esp_nvs_stubs.c",
        "tests/native/retained_blob_store_nvs_adapter.c",
        "tests/native/mbedtls_md_stub.c",
        "tests/native/mock_esp_nvs.h",
        "tests/native/store_behavior_test.c",
        "tests/native/stubs/esp_attr.h",
        "tests/native/stubs/esp_err.h",
        "tests/native/stubs/esp_timer.h",
        "tests/native/stubs/freertos/FreeRTOS.h",
        "tests/native/stubs/freertos/semphr.h",
        "tests/native/stubs/mbedtls/md.h",
        "tests/native/stubs/nvs.h",
    }
    packet_hash = {
        "main/mesh/meshcore_packet_hash.c",
        "main/mesh/meshcore_packet_hash.h",
        "main/mesh/meshcore_wire.c",
        "main/mesh/meshcore_wire.h",
        "tests/native/meshcore_packet_hash_test.c",
        "tests/native/stubs/esp_err.h",
        "tests/native/stubs/mbedtls/md.h",
    }
    lifetime = {
        "main/mesh/meshcore_lifetime.h",
        "tests/native/meshcore_lifetime_test.c",
    }
    rx_admission = {
        "main/mesh/meshcore_rx_admission.c",
        "main/mesh/meshcore_rx_admission.h",
        "tests/native/meshcore_rx_admission_test.c",
    }
    time_service = {
        "main/app/identity_state.h",
        "main/app/settings_envelope.c",
        "main/app/settings_envelope.h",
        "main/app/settings_model.h",
        "main/app/settings_protocol_migration.c",
        "main/app/settings_protocol_migration.h",
        "main/app/settings_time_checkpoint.c",
        "main/app/settings_time_checkpoint.h",
        "main/hal/rp2040_bridge.h",
        "main/mesh/meshcore_radio_profile.h",
        "main/mesh/store_lock.h",
        "main/platform/time_display.c",
        "main/platform/time_display.h",
        "main/platform/time_service.c",
        "main/platform/time_service.h",
        "main/platform/time_service_core.c",
        "main/platform/time_service_core.h",
        "main/storage/map_tile_store.h",
        "main/storage/retained_blob_store.h",
        "main/storage/storage_status.h",
        "tests/native/esp_nvs_stubs.c",
        "tests/native/mock_esp_nvs.h",
        "tests/native/stubs/esp_err.h",
        "tests/native/stubs/esp_netif_sntp.h",
        "tests/native/stubs/esp_timer.h",
        "tests/native/stubs/freertos/FreeRTOS.h",
        "tests/native/stubs/freertos/semphr.h",
        "tests/native/stubs/nvs.h",
        "tests/native/stubs/sys/time.h",
        "tests/native/time_service_checkpoint_integration_test.c",
    }
    all_dependencies = set(matrix["source_pins"])
    suite_sets = [
        admin,
        runtime,
        trace,
        migration,
        time_service,
        text_admission,
        ack_delivery,
        advert_admission,
        packet_hash,
        lifetime,
        rx_admission,
    ]
    suite_specs = conformance.production_semantic_suite_specs()
    commands = [
        [conformance._canonical_command_argument(argument) for argument in command]
        for command in conformance.production_semantic_dependency_command_plan(cc)
    ]
    return {
        "verified": True,
        "compiler_generated": True,
        "dependency_mode": "-MM",
        "dependency_invocation_scope": "one_translation_unit_per_command",
        "source_hash_mode": "canonical_lf_text_sha256",
        "suite_count": len(suite_sets),
        "translation_unit_count": sum(
            len(spec["sources"]) for spec in suite_specs
        ),
        "dependency_count": len(all_dependencies),
        "pinned_source_count": len(all_dependencies),
        "dependencies": sorted(all_dependencies),
        "suite_dependencies": [
            {
                "id": suite_id,
                "translation_unit_count": len(spec["sources"]),
                "dependency_count": len(dependencies),
                "dependencies": sorted(dependencies),
            }
            for suite_id, dependencies, spec in zip(
                conformance.EXPECTED_WP05_PRODUCTION_SUITES,
                suite_sets,
                suite_specs,
                strict=True,
            )
        ],
        "commands": commands,
        "unpinned_dependencies": [],
        "unused_pins": [],
        "hash_mismatches": [],
        "failures": [],
    }


def toolchain_receipt(build_inputs_path: Path, commit: str) -> dict:
    metadata = verify_ci_tool_inputs.load_metadata(build_inputs_path)
    contract = verify_ci_tool_inputs.clang_contract(metadata)
    executable = contract["executable"]
    return {
        "schema": 1,
        "kind": verify_ci_tool_inputs.RECEIPT_KIND,
        "ok": True,
        "source_commit": commit,
        "metadata": {
            "path": ".github/d1l-build-inputs.json",
            "sha256": verify_ci_tool_inputs.sha256_file(build_inputs_path),
        },
        "runner": contract["runner"],
        "architecture": contract["architecture"],
        "clang": {
            "version": contract["version"],
            "package_name": contract["package_name"],
            "package_version": contract["package_version"],
            "archive": {**contract["archive"], "verified": True},
            "executables": {
                role: {
                    "command": executable["commands"][index],
                    "resolved_path": executable["resolved_path"],
                    "sha256": executable["sha256"],
                    "size": executable["size"],
                    "verified": True,
                }
                for index, role in enumerate(("cc", "cxx"))
            },
            "bytes_verified": True,
        },
    }


def completed_signed_advert_runtime_report(
    commit: str,
    *,
    generated_at: datetime | None = None,
) -> dict:
    manifest = signed_runtime.load_manifest()
    dependency = manifest["external_dependency"]
    repository_files = {
        path: {
            "expected_sha256": digest,
            "actual_sha256": digest,
            "matched": True,
        }
        for group in signed_runtime._source_groups(manifest)
        for path, digest in group.items()
    }
    return {
        "schema_version": 1,
        "artifact_type": signed_runtime.SIGNED_ADVERT_ARTIFACT_TYPE,
        "status": "pass",
        "passed": True,
        "execution_complete": True,
        "generated_at": (generated_at or datetime.now(timezone.utc)).isoformat(),
        "work_package": "WP-04",
        "capability": manifest["capability"],
        "coverage_boundary": manifest["coverage_boundary"],
        "wp04_closure_eligible": False,
        "closure_ready": False,
        "repository": {
            "verified": True,
            "repository_commit": commit,
            "expected_repository_commit": commit,
            "upstream_commit": manifest["upstream"]["commit"],
            "gitlink_commit": manifest["upstream"]["commit"],
            "source_hash_mode": manifest["source_hash_mode"],
            "files": repository_files,
        },
        "external_archive": {
            "verified": True,
            "source": dependency["archive_url"],
            "url": dependency["archive_url"],
            "size": dependency["archive_size"],
            "sha256": dependency["archive_sha256"],
            "version": dependency["version"],
            "registry_version_id": dependency["registry_version_id"],
            "release_commit": dependency["release_commit"],
        },
        "external_sources": {
            "verified": True,
            "files": {
                path: {"sha256": digest, "size": 1}
                for path, digest in dependency["sources"].items()
            },
        },
        "sanitizers_enabled": True,
        "sanitizer_policy": manifest["sanitizer_policy"],
        "full_ubsan_clean": True,
        "commands": signed_runtime.command_plan(
            "clang-18",
            "clang++-18",
            "/tmp/crypto",
            "/tmp/build",
            sanitize=True,
        ),
        "result": manifest["expected_result"],
        "assertions": manifest["assertions"],
        "residual_gaps": manifest["residual_gaps"],
    }


def completed_report(
    commit: str,
    build_inputs_path: Path,
    *,
    generated_at: datetime | None = None,
) -> dict:
    source_files = {
        name: {
            "expected_sha256": digest,
            "actual_sha256": digest,
            "matched": True,
        }
        for name, digest in conformance.EXPECTED_UPSTREAM["sources"].items()
    }
    seeds = [
        {
            "name": f"seed_{index}",
            "size": index + 1,
            "sha256": f"{index + 1:064x}",
        }
        for index in range(conformance.EXPECTED_FUZZ_CORPUS["seed_count"])
    ]
    advert_seeds = [
        {
            "name": f"advert_seed_{index}",
            "size": index + 1,
            "sha256": f"{index + 101:064x}",
        }
        for index in range(
            conformance.EXPECTED_ADVERT_FUZZ_CORPUS["seed_count"]
        )
    ]
    temporary = "/tmp/d1l-meshcore-conformance-fixture"
    commands = conformance.command_plan(
        "clang-18",
        "clang++-18",
        temporary,
    )
    oracle_manifest = json.loads(
        conformance.ORACLE_MANIFEST_PATH.read_text(encoding="utf-8")
    )
    semantic_matrix = json.loads(
        conformance.WP05_SEMANTIC_MATRIX_PATH.read_text(encoding="utf-8")
    )
    semantic_summary = conformance.summarize_wp05_semantic_matrix(
        semantic_matrix, oracle_manifest
    )
    semantic_summary["source_verification"] = {
        "verified": True,
        "pins_verified": True,
        "dependency_closure_verified": True,
        "dependency_verification": semantic_dependency_receipt(
            semantic_matrix, "clang-18"
        ),
        "source_hash_mode": "canonical_lf_text_sha256",
        "files": {
            relative: {
                "expected_sha256": digest,
                "actual_sha256": digest,
                "matched": True,
            }
            for relative, digest in semantic_matrix["source_pins"].items()
        },
        "failures": [],
    }
    semantic_summary["result"] = {
        "passed": True,
        "coverage_boundary": "declared_wp05_host_semantic_matrix",
        "closure_ready": False,
        "sanitizers": ["address", "undefined"],
        "suite_count": len(semantic_matrix["production_host_suites"]),
        "scenario_count": sum(
            len(suite["scenario_ids"])
            for suite in semantic_matrix["production_host_suites"]
        ),
        "suites": [
            {
                "id": suite["id"],
                "passed": True,
                "scenario_count": len(suite["scenario_ids"]),
                "invocation_count": len(suite["invocations"]),
                "scenario_ids": suite["scenario_ids"],
                "expected_stdout": suite["expected_stdout"],
            }
            for suite in semantic_matrix["production_host_suites"]
        ],
        "failures": 0,
    }
    signed_advert_report = completed_signed_advert_runtime_report(
        commit,
        generated_at=generated_at,
    )
    return {
        "schema_version": 1,
        "artifact_type": "d1l_meshcore_wire_conformance",
        "generated_at": (generated_at or datetime.now(timezone.utc)).isoformat().replace(
            "+00:00", "Z"
        ),
        "passed": True,
        "status": "pass",
        "execution_complete": True,
        "coverage_boundary": "wire_envelope_only",
        "coverage_level": "wire_envelope_only",
        "semantic_coverage_level": "partial_declared_host_semantics",
        "closure_ready": False,
        "issue_65_closure_eligible": False,
        "excluded_coverage": {
            "crypto_vectors": 0,
            "retained_state_vectors": 0,
            "hardware_vectors": 0,
            "rf_vectors": 0,
            "packet_semantics_vectors": 0,
        },
        "scope": {
            "vector_oracle": "pinned_Packet_wire_read_write_only",
            "fuzz_target": "local_wire_decoder_only",
            "semantic_fuzz_targets": ["local_advert_parser"],
            "advert_parser_fuzz_covered": True,
            "packet_semantics_covered": False,
            "bounded_production_semantics_covered": True,
            "crypto_oracle_available": False,
        },
        "requested": {
            "commit": commit,
            "seed": conformance.DEFAULT_SEED,
            "fuzz_runs": conformance.DEFAULT_RUNS,
            "advert_fuzz_runs": conformance.DEFAULT_RUNS,
            "sanitizers": ["address", "undefined"],
        },
        "sanitizer_policy": conformance.ED25519_SANITIZER_POLICY,
        "full_ubsan_clean": True,
        "sanitizer_policy_passed": True,
        "vector_matrix": conformance.EXPECTED_VECTOR_MATRIX,
        "payload_version_gate": conformance.EXPECTED_PAYLOAD_VERSION_GATE,
        "wp05_semantic_matrix": semantic_summary,
        "signed_advert_runtime": conformance.bind_signed_advert_runtime_receipt(
            signed_advert_report,
            commit,
        ),
        "source_verification": {
            "verified": True,
            "repository_commit": commit,
            "expected_repository_commit": commit,
            "checkout_commit": commit,
            "upstream_commit": conformance.EXPECTED_UPSTREAM["commit"],
            "gitlink_mode": "160000",
            "gitlink_type": "commit",
            "gitlink_commit": conformance.EXPECTED_UPSTREAM["commit"],
            "expected_upstream_commit": conformance.EXPECTED_UPSTREAM["commit"],
            "source_hash_mode": "canonical_lf_text_sha256",
            "source_files": source_files,
            "allowlisted_source_sha256_verified": True,
            "failures": [],
        },
        "corpus": {
            "path": conformance.EXPECTED_FUZZ_CORPUS["manifest"],
            "manifest_sha256": conformance.EXPECTED_FUZZ_CORPUS["sha256"],
            "coverage_boundary": "local_wire_decoder_only",
            "seed_count": len(seeds),
            "seeds": seeds,
            "verified": True,
        },
        "advert_corpus": {
            "path": conformance.EXPECTED_ADVERT_FUZZ_CORPUS["manifest"],
            "manifest_sha256": conformance.EXPECTED_ADVERT_FUZZ_CORPUS[
                "sha256"
            ],
            "coverage_boundary": "local_advert_parser",
            "seed_count": len(advert_seeds),
            "seeds": advert_seeds,
            "verified": True,
        },
        "commands": commands,
        "fuzz_command": [f"{temporary}/meshcore_wire_fuzz", "-runs=100000"],
        "advert_fuzz_command": [
            f"{temporary}/meshcore_advert_fuzz",
            "-runs=100000",
        ],
        "vector_result": {
            "passed": True,
            "coverage_boundary": "wire_envelope_only",
            "issue_65_closure_eligible": False,
            "vectors": {
                "upstream_to_local": 576,
                "local_to_upstream": 576,
                "total": 1152,
            },
            "path_length_encodings": {"tested": 256, "valid": 119, "invalid": 137},
            "payload_version_gate": {
                "tested": 4,
                "accepted_v1": 1,
                "rejected_future": 3,
                "preserved_rejections": 3,
            },
            "undersized_encoder_capacities": 254,
            "failures": 0,
        },
        "fuzz_result": {
            "passed": True,
            "engine": "libFuzzer",
            "seed": conformance.DEFAULT_SEED,
            "requested_runs": conformance.DEFAULT_RUNS,
            "completed_runs": conformance.DEFAULT_RUNS,
            "duration_ms": 1000,
            "max_input_bytes": 255,
            "sanitizers": ["address", "undefined"],
            "artifact_prefix": "/tmp/findings-fixture",
            "finding_files": [],
            "findings": 0,
        },
        "advert_fuzz_result": {
            "passed": True,
            "engine": "libFuzzer",
            "target": "local_advert_parser",
            "invalid_output_policy": "canonical_zeroed_with_type_N",
            "seed": conformance.DEFAULT_SEED,
            "requested_runs": conformance.DEFAULT_RUNS,
            "completed_runs": conformance.DEFAULT_RUNS,
            "duration_ms": 500,
            "max_input_bytes": 33,
            "sanitizers": ["address", "undefined"],
            "artifact_prefix": "/tmp/advert-findings-fixture",
            "finding_files": [],
            "findings": 0,
        },
        "compiler": {
            "cc": "Ubuntu clang version 18.1.3 (1ubuntu1)",
            "cxx": "Ubuntu clang version 18.1.3 (1ubuntu1)",
        },
        "toolchain": toolchain_receipt(build_inputs_path, commit),
        "sanitizer_errors": 0,
        "memory_errors": 0,
        "zero_overrun": True,
        "zero_corruption": True,
        "failure": None,
    }


def write_completed_report(
    path: Path,
    commit: str,
    build_inputs_path: Path,
    *,
    generated_at: datetime | None = None,
) -> dict:
    report = completed_report(
        commit,
        build_inputs_path,
        generated_at=generated_at,
    )
    path.write_text(json.dumps(report, sort_keys=True), encoding="ascii")
    return report
