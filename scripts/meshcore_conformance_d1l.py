#!/usr/bin/env python3
"""Run the bounded D1L/Pinned-MeshCore host conformance gate.

This covers the wire envelope, the explicitly versioned oracle capabilities,
and the bounded production Admin, contact-TRACE, and protocol-timestamp host
semantic matrix. It is not a full delivery, RF, hardware, or complete MeshCore
claim.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence

if __package__:
    from .meshcore_signed_advert_runtime_d1l import (
        SIGNED_ADVERT_ARTIFACT_TYPE,
        SIGNED_ADVERT_EVIDENCE_PROFILE,
        canonicalize_release_report as canonicalize_signed_advert_runtime,
        validate_completed_report as validate_signed_advert_runtime,
    )
    from .verify_ci_tool_inputs import (
        load_metadata as load_build_inputs,
        validate_clang_receipt,
    )
else:
    from meshcore_signed_advert_runtime_d1l import (  # type: ignore[no-redef]
        SIGNED_ADVERT_ARTIFACT_TYPE,
        SIGNED_ADVERT_EVIDENCE_PROFILE,
        canonicalize_release_report as canonicalize_signed_advert_runtime,
        validate_completed_report as validate_signed_advert_runtime,
    )
    from verify_ci_tool_inputs import (  # type: ignore[no-redef]
        load_metadata as load_build_inputs,
        validate_clang_receipt,
    )


ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "tests" / "meshcore_conformance" / "manifest.json"
CORPUS_PATH = ROOT / "tests" / "meshcore_conformance" / "corpus.json"
ADVERT_CORPUS_PATH = (
    ROOT / "tests" / "meshcore_conformance" / "advert_corpus.json"
)
HARNESS_PATH = ROOT / "tests" / "meshcore_conformance" / "meshcore_wire_conformance.cpp"
FUZZ_PATH = ROOT / "tests" / "meshcore_conformance" / "meshcore_wire_fuzz.cpp"
ADVERT_FUZZ_PATH = (
    ROOT / "tests" / "meshcore_conformance" / "meshcore_advert_fuzz.cpp"
)
WIRE_SOURCE = ROOT / "main" / "mesh" / "meshcore_wire.c"
LOCAL_ADVERT_SOURCE = ROOT / "main" / "mesh" / "advert_data.c"
PACKET_SOURCE = ROOT / "third_party" / "MeshCore" / "src" / "Packet.cpp"
ADVERT_DATA_SOURCE = (
    ROOT
    / "third_party"
    / "MeshCore"
    / "src"
    / "helpers"
    / "AdvertDataHelpers.cpp"
)
ORACLE_UTILS_SOURCE = ROOT / "third_party" / "MeshCore" / "src" / "Utils.cpp"
ORACLE_AES_ROOT = (
    ROOT
    / "third_party"
    / "sensecap_indicator_esp32"
    / "components"
    / "LoRaWAN"
    / "soft-se"
)
ORACLE_AES_SOURCE = ORACLE_AES_ROOT / "aes.c"
ORACLE_MANIFEST_PATH = ROOT / "tests" / "meshcore_oracle" / "manifest.json"
ORACLE_COVERAGE_MANIFEST_PATH = (
    ROOT / "tests" / "meshcore_oracle" / "coverage_manifest.json"
)
WP05_SEMANTIC_MATRIX_PATH = (
    ROOT / "tests" / "meshcore_oracle" / "wp05_semantic_matrix.json"
)
ORACLE_ADAPTER_PATH = ROOT / "tests" / "meshcore_oracle" / "meshcore_oracle.cpp"
ORACLE_VECTORS_PATH = (
    ROOT / "tests" / "meshcore_oracle" / "meshcore_oracle_vectors.cpp"
)
ORACLE_ADMIN_SESSION_FIXTURE_PATH = (
    ROOT / "tests" / "meshcore_oracle" / "meshcore_admin_session_fixture.cpp"
)
ED25519_ROOT = ROOT / "third_party" / "MeshCore" / "lib" / "ed25519"
ED25519_DEFINED_ROOT = ROOT / "overlays" / "meshcore_ed25519_defined"
ED25519_VERIFY_SOURCES = [
    ED25519_DEFINED_ROOT / "fe.c",
    ED25519_DEFINED_ROOT / "ge.c",
    ED25519_DEFINED_ROOT / "sc.c",
    ED25519_ROOT / "sha512.c",
    ED25519_ROOT / "verify.c",
]
ED25519_VECTOR_PROVENANCE_SOURCES = [
    ED25519_ROOT / "key_exchange.c",
    ED25519_ROOT / "keypair.c",
    ED25519_ROOT / "sign.c",
]
ED25519_ORACLE_SOURCES = [
    *ED25519_VERIFY_SOURCES,
    *ED25519_VECTOR_PROVENANCE_SOURCES,
]
ED25519_SANITIZER_POLICY = {
    "requested_sanitizers": ["address", "undefined"],
    "full_ubsan_clean": True,
    "exceptions": [],
    "source_level_remediation": {
        "id": "BLK-WP04-ED25519-SHIFT-UB-20260714",
        "status": "resolved",
        "blocks_execution": False,
        "blocks_release": False,
        "upstream_commit": "e8d3c53ba1ea863937081cd0caad759b832f3028",
        "overlay_path": "overlays/meshcore_ed25519_defined",
        "validator": "scripts/validate_ed25519_defined_overlay.py",
        "transformed_expressions": 215,
    },
}
DEFAULT_SEED = 0xD1C065
DEFAULT_RUNS = 100_000
ORACLE_ABI_VERSION = 2
ORACLE_CORPUS_VERSION = 24
ORACLE_COVERAGE_BOUNDARY = (
    "pinned_upstream_packet_advert_group_dm_expected_ack_path_return_"
    "route_codes_ack_trace_and_signed_advert_creation_strict_verification_"
    "and_anonymous_login_request_and_regular_request_response_crypto_and_"
    "strict_identity_shared_secret_derivation_and_canonical_login_response_"
    "packets_and_login_password_authorization_fixtures_and_existing_acl_"
    "blank_login_reuse_fixtures_and_authorized_login_acl_transition_fixtures_"
    "and_authenticated_request_replay_transition_fixtures_and_authenticated_"
    "text_replay_response_session_orchestration_fixtures_and_login_response_"
    "creation_dispatch_orchestration_fixtures_and_signed_advert_dispatch_"
    "transition_fixtures_and_signed_advert_creation_send_orchestration_fixtures"
)
BUILD_INPUTS_PATH = ROOT / ".github" / "d1l-build-inputs.json"
CANONICAL_EVIDENCE_PROFILE = "d1l_meshcore_wire_conformance_package_v1"
VOLATILE_RUN_RECEIPT_FIELDS = (
    "/generated_at",
    "/commands/*/checkout-and-temporary-build-paths",
    "/fuzz_command/temporary-build-and-findings-paths",
    "/fuzz_result/duration_ms",
    "/fuzz_result/artifact_prefix",
    "/advert_fuzz_command/temporary-build-and-findings-paths",
    "/advert_fuzz_result/duration_ms",
    "/advert_fuzz_result/artifact_prefix",
)
EXPECTED_UPSTREAM = {
    "name": "MeshCore",
    "path": "third_party/MeshCore",
    "commit": "e8d3c53ba1ea863937081cd0caad759b832f3028",
    "sources": {
        "src/MeshCore.h": "51d93fe4c18608efa827625639a68721cedd9c15a83d55e3b86629cd5710ba8a",
        "src/Packet.cpp": "6f401a4abe57b67aae6ce54172a8cd5cfa64f801c2186894d4bf28aaf30b52e0",
        "src/Packet.h": "dae1e63021df0b85ccd83bd409993e302daf768509a7a28e8485d02344528d9f",
    },
}
EXPECTED_PRODUCTION_WIRE_SOURCES = [
    "main/mesh/meshcore_wire.c",
    "main/mesh/meshcore_wire.h",
]
EXPECTED_FUZZ_CORPUS = {
    "manifest": "tests/meshcore_conformance/corpus.json",
    "sha256": "ad0d3d03f1699a86c61812b5307b0bd47ffd50cd9fdbbbd69f73f1df893eef25",
    "seed_count": 5,
}
EXPECTED_ADVERT_FUZZ_CORPUS = {
    "manifest": "tests/meshcore_conformance/advert_corpus.json",
    "sha256": "9a85e4377e1bc59542e5509edb105225fe303578f5f5a860f217b0c619513b1c",
    "seed_count": 8,
}

EXPECTED_VECTORS = {
    "payload_types": 7,
    "route_types": 4,
    "path_cases": 9,
    "payload_lengths": 2,
    "local_to_upstream": 504,
    "upstream_to_local": 504,
    "total": 1008,
}
EXPECTED_PAYLOAD_VERSION_GATE = {
    "permissive_envelope_versions": [0, 1, 2, 3],
    "accepted_v1_versions": [0],
    "rejected_future_versions": [1, 2, 3],
    "preserve_output_on_reject": True,
}
EXPECTED_VECTOR_MATRIX = {
    "payload_types": [
        {"name": "TXT", "code": 2},
        {"name": "ACK", "code": 3},
        {"name": "ADVERT", "code": 4},
        {"name": "GRP_TXT", "code": 5},
        {"name": "PATH", "code": 8},
        {"name": "TRACE", "code": 9},
        {"name": "MULTIPART", "code": 10},
    ],
    "route_types": [
        {"name": "TRANSPORT_FLOOD", "code": 0},
        {"name": "FLOOD", "code": 1},
        {"name": "DIRECT", "code": 2},
        {"name": "TRANSPORT_DIRECT", "code": 3},
    ],
    "path_cases": [
        {"hash_bytes": 1, "hop_count": 0},
        {"hash_bytes": 1, "hop_count": 1},
        {"hash_bytes": 1, "hop_count": 63},
        {"hash_bytes": 2, "hop_count": 0},
        {"hash_bytes": 2, "hop_count": 1},
        {"hash_bytes": 2, "hop_count": 32},
        {"hash_bytes": 3, "hop_count": 0},
        {"hash_bytes": 3, "hop_count": 1},
        {"hash_bytes": 3, "hop_count": 21},
    ],
    "payload_lengths": [1, 184],
}
EXPECTED_FUZZ = {
    "engine": "libFuzzer",
    "runs": DEFAULT_RUNS,
    "seed": DEFAULT_SEED,
    "max_input_bytes": 255,
    "sanitizers": ["address", "undefined"],
}
EXPECTED_ADVERT_FUZZ = {
    "engine": "libFuzzer",
    "runs": DEFAULT_RUNS,
    "seed": DEFAULT_SEED,
    "max_input_bytes": 33,
    "sanitizers": ["address", "undefined"],
    "target": "local_advert_parser",
    "production_sources": [
        "main/mesh/advert_data.c",
        "main/mesh/advert_data.h",
    ],
    "invalid_output_policy": "canonical_zeroed_with_type_N",
}
EXPECTED_WP05_REQUIREMENT_STATUSES = {
    "signed_adverts_names_roles_location": "partial",
    "public_and_configured_channels": "implemented",
    "dm_attempts_0_through_255": "implemented",
    "shared_secret_derivation": "implemented",
    "mac_authentication_failure": "implemented",
    "direct_flood_transport_codes": "implemented",
    "ack_multi_ack_and_flood_ack_path": "implemented",
    "path_hash_sizes_counts_and_maximums": "implemented",
    "request_response_login_keepalive_status": "partial",
    "trace_and_path_discovery": "partial",
    "replayed_advert_timestamp": "implemented",
    "duplicate_packet_and_hash": "implemented",
    "truncated_oversize_and_empty_payload": "implemented",
    "malformed_utf8_and_embedded_nul": "implemented",
    "self_message_unknown_peer_and_channel": "implemented",
    "lifetime_and_expiry_boundaries": "implemented",
}
EXPECTED_WP05_FUZZ_TARGET_STATUSES = {
    "raw_wire_decoder": "implemented",
    "semantic_packet_parser": "implemented",
    "decrypt_auth_path": "implemented",
    "advert_parser": "implemented",
    "dm_session_transition_input": "implemented",
    "retained_envelope_decoder": "implemented",
    "route_path_parser": "implemented",
    "usb_protocol_command_parser": "implemented",
}
EXPECTED_WP05_PRODUCTION_SUITES = {
    "admin_dispatch": {
        "binary": "meshcore_admin_dispatch_semantic",
        "scenario_count": 4,
        "invocation_count": 1,
        "expected_stdout": "meshcore_admin_dispatch_test: ok",
    },
    "runtime_queue_fairness": {
        "binary": "meshcore_runtime_guard_semantic",
        "scenario_count": 5,
        "invocation_count": 1,
        "expected_stdout": "native mesh runtime guards: ok",
    },
    "contact_trace": {
        "binary": "meshcore_trace_semantic",
        "scenario_count": 3,
        "invocation_count": 1,
        "expected_stdout": "meshcore_trace_test: ok",
    },
    "legacy_protocol_timestamp_migration": {
        "binary": "settings_protocol_migration_semantic",
        "scenario_count": 8,
        "invocation_count": 1,
        "expected_stdout": "native legacy protocol migration: ok",
    },
    "time_service_migration_integration": {
        "binary": "time_service_migration_semantic",
        "scenario_count": 6,
        "invocation_count": 6,
        "expected_stdout": "native truthful-time checkpoint integration: ok",
    },
    "text_admission": {
        "binary": "meshcore_text_plaintext_semantic",
        "scenario_count": 8,
        "invocation_count": 1,
        "expected_stdout": "native MeshCore text plaintext: ok",
    },
    "ack_delivery_correlation": {
        "binary": "meshcore_ack_completion_semantic",
        "scenario_count": 8,
        "invocation_count": 1,
        "expected_stdout": "native MeshCore ACK completion: ok",
    },
    "advert_replay_admission": {
        "binary": "meshcore_advert_admission_semantic",
        "scenario_count": 12,
        "invocation_count": 1,
        "expected_stdout": "native node/contact store behavior: ok",
    },
    "packet_hash_and_advert_dedupe": {
        "binary": "meshcore_packet_hash_semantic",
        "scenario_count": 7,
        "invocation_count": 1,
        "expected_stdout": "native MeshCore packet hash: ok",
    },
    "lifetime_boundaries": {
        "binary": "meshcore_lifetime_semantic",
        "scenario_count": 8,
        "invocation_count": 1,
        "expected_stdout": "native MeshCore lifetime boundaries: ok",
    },
    "channel_and_peer_rx_admission": {
        "binary": "meshcore_rx_admission_semantic",
        "scenario_count": 6,
        "invocation_count": 1,
        "expected_stdout": "native MeshCore RX admission: ok",
    },
}
EXPECTED_WP05_COMPANION_UPSTREAM_SUITES = {
    "pinned_signed_advert_runtime": {
        "artifact_type": SIGNED_ADVERT_ARTIFACT_TYPE,
        "capability": "identity_signed_advert_semantic_runtime",
        "case_ids": [
            "distinct_equal_timestamp_rejected",
            "distinct_older_timestamp_rejected",
            "strictly_newer_timestamp_accepted",
            "uint32_max_timestamp_accepted",
            "wrapped_zero_timestamp_rejected",
            "real_simple_mesh_tables",
            "upstream_d1l_packet_hash_match",
            "route_path_transport_variants_suppressed",
            "trace_path_length_little_endian_match",
            "trace_path_bytes_excluded",
        ],
        "identical_wire_hash_case": "identical_wire_hash_suppressed",
        "closure_ready": False,
    },
}
EXPECTED_WP05_SOURCE_PATHS = {
    "main/app/identity_state.h",
    "main/app/settings_envelope.c",
    "main/app/settings_envelope.h",
    "main/app/settings_model.h",
    "main/app/settings_protocol_migration.c",
    "main/app/settings_protocol_migration.h",
    "main/app/settings_time_checkpoint.c",
    "main/app/settings_time_checkpoint.h",
    "main/hal/rp2040_bridge.h",
    "main/mesh/meshcore_admin_dispatch.c",
    "main/mesh/meshcore_admin_dispatch.h",
    "main/mesh/dm_delivery_state.c",
    "main/mesh/dm_delivery_state.h",
    "main/mesh/contact_store.c",
    "main/mesh/contact_store.h",
    "main/mesh/contact_uri.c",
    "main/mesh/contact_uri.h",
    "main/mesh/meshcore_command_guard.h",
    "main/mesh/meshcore_ack_completion.h",
    "main/mesh/meshcore_advert_admission.c",
    "main/mesh/meshcore_advert_admission.h",
    "main/mesh/meshcore_packet_hash.c",
    "main/mesh/meshcore_packet_hash.h",
    "main/mesh/meshcore_lifetime.h",
    "main/mesh/meshcore_dm_retry.h",
    "main/mesh/meshcore_path_state.c",
    "main/mesh/meshcore_path_state.h",
    "main/mesh/meshcore_radio_profile.h",
    "main/mesh/meshcore_rx_admission.c",
    "main/mesh/meshcore_rx_admission.h",
    "main/mesh/meshcore_runtime_guard.h",
    "main/mesh/meshcore_trace.h",
    "main/mesh/meshcore_text_plaintext.c",
    "main/mesh/meshcore_text_plaintext.h",
    "main/mesh/meshcore_wire.c",
    "main/mesh/meshcore_wire.h",
    "main/mesh/store_lock.h",
    "main/mesh/node_store.c",
    "main/mesh/node_store.h",
    "main/mesh/user_text.c",
    "main/mesh/user_text.h",
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
    "tests/native/mbedtls_md_stub.c",
    "tests/native/meshcore_admin_dispatch_test.c",
    "tests/native/meshcore_ack_completion_test.c",
    "tests/native/meshcore_lifetime_test.c",
    "tests/native/meshcore_runtime_guard_test.c",
    "tests/native/meshcore_rx_admission_test.c",
    "tests/native/meshcore_packet_hash_test.c",
    "tests/native/meshcore_trace_test.c",
    "tests/native/meshcore_text_plaintext_test.c",
    "tests/native/mock_esp_nvs.h",
    "tests/native/settings_protocol_migration_test.c",
    "tests/native/stubs/esp_err.h",
    "tests/native/stubs/esp_attr.h",
    "tests/native/stubs/esp_netif_sntp.h",
    "tests/native/stubs/esp_timer.h",
    "tests/native/stubs/mbedtls/md.h",
    "tests/native/stubs/freertos/FreeRTOS.h",
    "tests/native/stubs/freertos/semphr.h",
    "tests/native/stubs/nvs.h",
    "tests/native/stubs/sys/time.h",
    "tests/native/time_service_checkpoint_integration_test.c",
    "tests/native/store_behavior_test.c",
}
SIGNED_ADVERT_REPLAY_OUTCOME_KEYS = (
    "distinct_equal_timestamp_rejected",
    "distinct_older_timestamp_rejected",
    "strictly_newer_timestamp_accepted",
    "uint32_max_timestamp_accepted",
    "wrapped_zero_timestamp_rejected",
)
SIGNED_ADVERT_CANONICAL_FIELDS = {
    "schema_version",
    "artifact_type",
    "evidence_profile",
    "status",
    "passed",
    "execution_complete",
    "work_package",
    "capability",
    "coverage_boundary",
    "wp04_closure_eligible",
    "closure_ready",
    "repository",
    "external_archive",
    "external_sources",
    "sanitizers_enabled",
    "sanitizer_policy",
    "full_ubsan_clean",
    "result",
    "assertions",
    "residual_gaps",
}
SIGNED_ADVERT_PACKET_HASH_OUTCOME_KEYS = (
    "real_simple_mesh_tables",
    "upstream_d1l_packet_hash_match",
    "route_path_transport_variants_suppressed",
    "trace_path_length_little_endian_match",
    "trace_path_bytes_excluded",
)
EXPECTED_ORACLE_CAPABILITIES = [
    {
        "id": "packet_envelope",
        "status": "implemented",
        "owner": "pinned_upstream",
        "semantic": False,
    },
    {
        "id": "advert_data_fields",
        "status": "implemented",
        "owner": "pinned_upstream",
        "semantic": True,
    },
    {
        "id": "signed_advert_verification",
        "status": "implemented",
        "owner": "pinned_d1l_production_ed25519",
        "semantic": True,
        "scope": "message_layout_and_signature_verification_only_no_mesh_dispatch",
    },
    {
        "id": "ed25519_point_validation",
        "status": "implemented",
        "owner": "pinned_d1l_production_ed25519",
        "semantic": True,
        "scope": "canonical_encoding_and_low_order_rejection_for_advert_public_key_and_signature_r",
    },
    {
        "id": "direct_flood_headers",
        "status": "implemented",
        "owner": "pinned_source_golden_vectors",
        "semantic": True,
        "scope": "non_trace_direct_flood_and_zero_hop_headers",
    },
    {
        "id": "ack_frames",
        "status": "implemented",
        "owner": "pinned_source_golden_vectors",
        "semantic": True,
        "scope": "simple_and_multipart_payload_framing_only",
    },
    {
        "id": "trace_source_frames",
        "status": "implemented",
        "owner": "pinned_source_golden_vectors",
        "semantic": True,
        "scope": "initial_outbound_direct_flags_zero_trace_framing_only",
    },
    {
        "id": "identity_signed_advert",
        "status": "pending",
        "owner": "unassigned",
        "blocked_by": (
            "external_unpinned_ed25519_verifier_and_concrete_route_runtime_"
            "execution"
        ),
        "blocked_scope": (
            "upstream_identity_verifier_parity_and_concrete_contact_table_"
            "path_queue_dispatch_execution"
        ),
        "blocker_receipt": {
            "id": "BLK-WP04-IDENTITY-DISPATCH-20260713",
            "status": "open",
            "observed_at": "2026-07-13",
            "pinned_sources": {
                "third_party/MeshCore/src/Identity.cpp": (
                    "21e7b533928f2d7dd5b016c048b5c4971db6d475077c793947c17574dd057bd0"
                ),
                "third_party/MeshCore/src/Dispatcher.cpp": (
                    "30b8e49a94a281e7d0c60c037d3e68d3d8882b4c82c6c98b15df981a2ce0116e"
                ),
                "third_party/MeshCore/src/Mesh.cpp": (
                    "e39aa2ca27cc4d6b1d19c43aeccb5915098aeb5b6a5fc9be98d9fa557a54fd7e"
                ),
            },
            "missing_inputs": [
                "exact external Ed25519.h verifier source used by Identity.cpp is absent from the pinned MeshCore gitlink",
                "concrete packet-manager, mesh-table, contact, encoded-path, queue, clock, delay, and dispatch runtime execution fixtures",
                "persisted identity public/private-key consistency validation before advert signing",
            ],
            "blocks_execution": False,
            "unblocked_slice": (
                "signed_advert_packet_creation_dispatch_and_send_orchestration"
            ),
        },
    },
    {
        "id": "public_group_packets",
        "status": "implemented",
        "owner": "pinned_upstream_utils_vendored_aes_host_sha",
        "semantic": True,
        "scope": (
            "basechatmesh_setchannel_hash_and_nonempty_group_text_data_encrypt_"
            "mac_parse_only_no_mesh_dispatch_delivery_or_retained_state"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-PUBLIC-GROUP-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/src/Mesh.cpp",
                "third_party/MeshCore/src/Utils.cpp",
                "third_party/MeshCore/src/helpers/BaseChatMesh.cpp",
                "third_party/sensecap_indicator_esp32/components/LoRaWAN/soft-se/aes.c",
            ],
            "independent_kats": [
                "FIPS 180-4 SHA-256 abc",
                "RFC 4231 HMAC-SHA-256 test case 1",
                "FIPS 197 AES-128 cipher example",
            ],
            "vectors": {
                "roundtrip": 4,
                "invalid": 21,
                "crypto_adapter_kat": 3,
            },
        },
    },
    {
        "id": "dm_encrypt_decrypt",
        "status": "implemented",
        "owner": "pinned_mesh_datagram_basechat_layout_vendored_aes_host_sha",
        "semantic": True,
        "scope": (
            "plain_dm_create_authenticated_parse_with_caller_supplied_hashes_and_"
            "shared_secret_no_key_exchange_dispatch_ack_delivery_or_retained_state"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-DM-CRYPTO-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/src/Mesh.cpp",
                "third_party/MeshCore/src/Utils.cpp",
                "third_party/MeshCore/src/helpers/BaseChatMesh.cpp",
                "third_party/MeshCore/src/helpers/BaseChatMesh.h",
            ],
            "attempt_matrix": "0_through_255",
            "golden_vectors": (
                "attempt_0_and_255_exact_payloads_plus_attempt_matrix_and_"
                "maximum_and_exact_block_payload_sha256_digests"
            ),
            "vectors": {
                "roundtrip": 268,
                "invalid": 29,
            },
        },
    },
    {
        "id": "expected_ack_hash_and_ack_path",
        "status": "implemented",
        "owner": "pinned_basechat_ack_hash_mesh_ack_path_vendored_crypto",
        "semantic": True,
        "scope": (
            "plain_dm_expected_hash_all_lengths_defined_ack_bodies_and_"
            "authenticated_ack_path_with_caller_supplied_public_key_hashes_"
            "secret_and_rng_byte_no_dispatch_correlation_delivery_state_"
            "route_selection_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-EXPECTED-ACK-PATH-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/src/Mesh.cpp",
                "third_party/MeshCore/src/Utils.cpp",
                "third_party/MeshCore/src/helpers/BaseChatMesh.cpp",
            ],
            "return_path_encodings": ["zero", "one_byte", "two_byte", "three_byte"],
            "golden_vectors": (
                "four_exact_ack_bodies_one_exact_block_ack_hash_zero_path_"
                "exact_payload_and_four_path_payload_matrix_sha256"
            ),
            "undefined_upstream_boundary": (
                "normal_dm_5_plus_text_len_exact_aes_block_has_deterministic_"
                "expected_hash_but_uninitialized_full_ack_body_byte"
            ),
            "vectors": {
                "roundtrip": 4,
                "valid": 9,
                "invalid": 35,
            },
        },
    },
    {
        "id": "ack_dispatch_correlation_and_delivery",
        "status": "implemented",
        "owner": "d1l_production_exact_owner_ack_completion",
        "semantic": True,
        "scope": (
            "simple_multipart_and_ack_path_exact_active_owner_hash_and_"
            "awaiting_revision_retained_cas_durable_publish_persistence_"
            "reconcile_take_once_deadline_route_and_hash_effects_no_rf_"
            "hardware_or_release_closure"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP05-ACK-COMPLETION-20260716",
            "status": "implemented_host_only_pending_rf_and_retained_fault_evidence",
            "observed_at": "2026-07-16",
            "pinned_sources": [
                "main/mesh/dm_delivery_state.c",
                "main/mesh/dm_delivery_state.h",
                "main/mesh/meshcore_ack_completion.h",
                "main/mesh/meshcore_service.c",
                "tests/native/meshcore_ack_completion_test.c",
            ],
            "host_suite": "ack_delivery_correlation",
            "scenario_count": 8,
            "closure_ready": False,
        },
    },
    {
        "id": "route_selection_and_forwarding",
        "status": "pending",
        "owner": "unassigned",
        "blocked_by": (
            "deterministic_mesh_dispatch_packet_manager_tables_radio_rng_and_clock_fixtures"
        ),
    },
    {
        "id": "path_return_route_codes",
        "status": "implemented",
        "owner": "pinned_mesh_path_return_utils_and_route_golden_vectors",
        "semantic": True,
        "scope": (
            "general_authenticated_path_return_extra_and_no_extra_uniqueness_"
            "framing_with_caller_supplied_hash_secret_rng_and_route_inputs_no_"
            "identity_derivation_route_selection_dispatch_forwarding_contact_"
            "mutation_scheduling_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-PATH-RETURN-ROUTE-CODES-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/src/Mesh.cpp",
                "third_party/MeshCore/src/Utils.cpp",
                "third_party/MeshCore/src/Packet.cpp",
            ],
            "return_path_encodings": [
                "zero",
                "one_byte",
                "two_byte_maximum",
                "three_byte_maximum",
            ],
            "route_matrix": [
                "flood",
                "transport_flood",
                "direct",
                "zero_hop",
                "transport_zero_hop",
            ],
            "golden_vectors": (
                "zero_path_exact_payload_general_extra_and_no_extra_uniqueness_"
                "matrix_sha256_and_route_code_matrix"
            ),
            "vectors": {"roundtrip": 6, "invalid": 35},
        },
    },
    {
        "id": "trace_forwarding_and_path_discovery",
        "status": "pending",
        "owner": "d1l_production_contact_trace",
        "blocked_by": (
            "deterministic_identity_mesh_tables_radio_snr_and_clock_fixtures"
        ),
        "implemented_prerequisites": [
            "trace_source_frames",
            "fingerprint_only_current_boot_proven_contact_loop_dispatch",
        ],
        "production_receipt": {
            "id": "RCPT-WP04-CONTACT-TRACE-20260716",
            "status": "implemented_pending_actions_and_rf_evidence",
            "sources": [
                "main/mesh/meshcore_service.c",
                "main/mesh/meshcore_service.h",
                "main/mesh/meshcore_command_guard.h",
                "main/mesh/meshcore_runtime_guard.h",
                "main/mesh/meshcore_trace.h",
                "main/comms/usb_console.c",
            ],
            "scope": (
                "fingerprint_only_exact_canonical_contact_current_boot_proven_"
                "one_or_two_byte_route_loop_runtime_dispatch_width_aware_"
                "correlation_generic_one_two_four_eight_byte_wire_helpers_no_"
                "operator_path_three_byte_contact_route_ui_rf_evidence_"
                "hardware_evidence_or_closure"
            ),
        },
    },
    {
        "id": "login_request_response_admin",
        "status": "pending",
        "owner": "d1l_production_admin_dispatch",
        "blocked_by": (
            "exact_actions_build_runtime_and_physical_delivery_evidence"
        ),
        "implemented_prerequisites": [
            "anonymous_login_request_packets",
            "regular_request_response_packets",
            "identity_shared_secret_derivation",
            "canonical_login_response_packets",
            "login_password_authorization_fixtures",
            "existing_acl_blank_login_reuse_fixtures",
            "authorized_login_acl_transition_fixtures",
            "authenticated_request_replay_transition_fixtures",
            "authenticated_text_replay_response_session_fixtures",
            "login_response_creation_dispatch_orchestration_fixtures",
            "host_authenticated_admin_session_fixture",
            "production_admin_login_status_dispatch",
            "production_admin_single_owner_login_status_logout_dispatch",
            "production_bounded_queue_saturation_and_fairness",
        ],
        "host_only_receipt": "RCPT-WP04-HOST-ADMIN-SESSION-20260714",
        "production_receipt": {
            "id": "RCPT-WP06-ADMIN-COMMAND-OWNER-20260716",
            "status": "implemented_pending_actions_and_physical_evidence",
            "sources": [
                "main/mesh/meshcore_admin_dispatch.c",
                "main/mesh/meshcore_admin_dispatch.h",
                "main/mesh/meshcore_admin_runtime.c",
                "main/mesh/meshcore_admin_runtime.h",
                "main/mesh/meshcore_command_guard.h",
                "main/mesh/meshcore_identity_exchange.c",
                "main/mesh/meshcore_identity_exchange.h",
                "main/mesh/meshcore_runtime_guard.h",
                "main/mesh/meshcore_service.c",
                "main/mesh/meshcore_service.h",
            ],
            "scope": (
                "repeater_admin_login_and_get_status_only_direct_or_flood_"
                "outbound_and_direct_or_path_response_inbound_room_rejected_"
                "single_owner_bounded_login_status_logout_commands_request_"
                "deadline_queue_saturation_and_zeroization_guards_no_ui_"
                "credential_storage_or_remote_mutating_admin_commands"
            ),
        },
        "queue_fairness_receipt": {
            "id": "RCPT-WP06-QUEUE-FAIRNESS-20260716",
            "status": "implemented_host_only_pending_actions_hardware_rf_and_wp06_closure",
            "sources": [
                "main/comms/usb_console.c",
                "main/mesh/meshcore_command_guard.h",
                "main/mesh/meshcore_runtime_guard.h",
                "main/mesh/meshcore_service.c",
                "main/mesh/meshcore_service.h",
            ],
            "host_suite": "runtime_queue_fairness",
            "scenario_count": 5,
            "scope": (
                "separate_fixed_normal_and_priority_command_queues_exact_"
                "terminal_recovery_precedence_four_radio_event_and_two_"
                "priority_command_burst_bounds_synchronous_request_"
                "cancellation_credential_wipe_owner_recursion_guard_and_"
                "truthful_saturation_fairness_maintenance_telemetry_no_"
                "actions_hardware_rf_wp06_or_release_closure"
            ),
        },
    },
    {
        "id": "signed_advert_packet_creation",
        "status": "implemented",
        "owner": "pinned_mesh_createadvert_vendored_ed25519",
        "semantic": True,
        "scope": (
            "deterministic_seed_keypair_pre_route_packet_creation_and_"
            "authenticated_parse_only_no_identity_verifier_dispatch_replay_"
            "contact_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-SIGNED-ADVERT-PACKETS-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/src/Mesh.cpp",
                "third_party/MeshCore/lib/ed25519/keypair.c",
                "third_party/MeshCore/lib/ed25519/sign.c",
                "third_party/MeshCore/lib/ed25519/verify.c",
            ],
            "golden_vectors": (
                "empty_named_and_maximum_app_data_exact_payloads_with_flood_"
                "wire_roundtrip_and_authenticated_parse"
            ),
            "vectors": {"roundtrip": 3, "invalid": 23},
        },
    },
    {
        "id": "anonymous_login_request_packets",
        "status": "implemented",
        "owner": "pinned_basechat_anon_datagram_vendored_crypto",
        "semantic": True,
        "scope": (
            "canonical_room_nonroom_login_request_create_authenticated_parse_"
            "with_caller_supplied_hash_public_key_secret_time_and_room_mode_"
            "only_no_key_exchange_authorization_replay_response_session_"
            "dispatch_route_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-ANON-LOGIN-REQUEST-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/src/Mesh.cpp",
                "third_party/MeshCore/src/Utils.cpp",
                "third_party/MeshCore/src/helpers/BaseChatMesh.cpp",
            ],
            "independent_golden": (
                "two_exact_payloads_and_six_case_payload_matrix_sha256_from_"
                "independent_aes128_ecb_hmac_sha256_generation"
            ),
            "vectors": {"roundtrip": 6, "invalid": 35},
        },
    },
    {
        "id": "regular_request_response_packets",
        "status": "implemented",
        "owner": "pinned_mesh_regular_datagram_vendored_crypto",
        "semantic": True,
        "scope": (
            "authenticated_nonempty_req_response_create_parse_with_caller_"
            "supplied_type_hashes_secret_and_logical_length_only_no_schema_"
            "tag_authorization_replay_session_dispatch_route_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-REGULAR-REQUEST-RESPONSE-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/src/Mesh.cpp",
                "third_party/MeshCore/src/Utils.cpp",
            ],
            "independent_golden": (
                "two_exact_payloads_maximum_payload_sha256_and_six_case_"
                "payload_matrix_sha256_from_independent_aes128_ecb_hmac_"
                "sha256_generation"
            ),
            "vectors": {"roundtrip": 6, "invalid": 30},
        },
    },
    {
        "id": "identity_shared_secret_derivation",
        "status": "implemented",
        "owner": "pinned_ed25519_keypair_exchange_strict_peer_guard",
        "semantic": True,
        "scope": (
            "valid_input_seed_keypair_strict_peer_edwards_to_montgomery_shared_"
            "secret_with_zero_rejection_only_no_persisted_key_contact_auth_"
            "session_dispatch_route_production_integration_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-IDENTITY-SHARED-SECRET-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/lib/ed25519/keypair.c",
                "third_party/MeshCore/lib/ed25519/key_exchange.c",
                "main/mesh/ed25519_canonical.h",
            ],
            "independent_golden": (
                "five_symmetric_public_key_and_shared_secret_vectors_from_"
                "libsodium_ed25519_to_curve25519_conversion_and_scalarmult"
            ),
            "vectors": {"roundtrip": 5, "invalid": 9},
        },
    },
    {
        "id": "canonical_login_response_packets",
        "status": "implemented",
        "owner": "pinned_repeater_room_response_schema_vendored_crypto",
        "semantic": True,
        "scope": (
            "canonical_repeater_room_success_response_schema_authenticated_"
            "create_parse_with_caller_supplied_hashes_secret_time_permissions_"
            "uniqueness_no_password_acl_replay_session_transition_dispatch_"
            "route_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-CANONICAL-LOGIN-RESPONSE-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp",
                "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp",
                "third_party/MeshCore/src/Mesh.cpp",
                "third_party/MeshCore/src/Utils.cpp",
                "third_party/MeshCore/src/helpers/ClientACL.h",
            ],
            "independent_golden": (
                "repeater_admin_and_room_guest_exact_payloads_plus_ten_case_"
                "payload_matrix_sha256_from_independent_aes128_ecb_hmac_"
                "sha256_generation"
            ),
            "vectors": {"roundtrip": 10, "invalid": 34},
        },
    },
    {
        "id": "login_password_authorization_fixtures",
        "status": "implemented",
        "owner": "pinned_repeater_room_password_rules",
        "semantic": True,
        "scope": (
            "canonical_repeater_room_acl_miss_password_admin_guest_read_only_"
            "authorization_denial_only_no_existing_acl_contact_mutation_replay_"
            "secret_session_response_dispatch_route_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-LOGIN-PASSWORD-AUTHORIZATION-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp",
                "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp",
                "third_party/MeshCore/src/helpers/ClientACL.h",
                "third_party/MeshCore/src/helpers/CommonCLI.h",
            ],
            "vectors": {"valid": 16, "invalid": 17},
        },
    },
    {
        "id": "existing_acl_blank_login_reuse_fixtures",
        "status": "implemented",
        "owner": "pinned_clientacl_lookup_repeater_room_reuse_rules",
        "semantic": True,
        "scope": (
            "canonical_repeater_room_blank_password_existing_acl_full_key_"
            "first_match_reuse_response_secret_selection_and_client_out_path_"
            "mutation_boundary_only_no_storage_load_save_insert_evict_"
            "permission_secret_timestamp_activity_replay_session_response_"
            "creation_dispatch_route_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-EXISTING-ACL-BLANK-LOGIN-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/src/helpers/ClientACL.cpp",
                "third_party/MeshCore/src/helpers/ClientACL.h",
                "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp",
                "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp",
            ],
            "vectors": {"valid": 12, "invalid": 14},
        },
    },
    {
        "id": "authorized_login_acl_transition_fixtures",
        "status": "implemented",
        "owner": "pinned_clientacl_putclient_repeater_room_login_mutation_rules",
        "semantic": True,
        "scope": (
            "canonical_clientacl_full_key_reuse_append_least_active_non_admin_"
            "evict_and_repeater_room_authorized_login_replay_role_secret_"
            "timestamp_activity_room_state_dirty_and_flood_out_path_transition_"
            "projection_only_no_filesystem_full_path_bytes_unmodified_room_"
            "scheduling_identity_signature_secret_derivation_password_response_"
            "dispatch_route_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-AUTHORIZED-LOGIN-ACL-TRANSITION-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/src/helpers/ClientACL.cpp",
                "third_party/MeshCore/src/helpers/ClientACL.h",
                "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp",
                "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp",
            ],
            "vectors": {"valid": 16, "invalid": 13},
        },
    },
    {
        "id": "authenticated_request_replay_transition_fixtures",
        "status": "implemented",
        "owner": "pinned_repeater_room_authenticated_request_replay_rules",
        "semantic": True,
        "scope": (
            "canonical_repeater_room_prevalidated_authenticated_request_newer_"
            "equal_older_handler_commit_and_direct_keep_alive_record_session_"
            "transition_projection_only_no_request_schema_handler_execution_"
            "response_hash_creation_storage_identity_secret_dispatch_route_"
            "schedule_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-AUTHENTICATED-REQUEST-REPLAY-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp",
                "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp",
                "third_party/MeshCore/src/helpers/ClientACL.h",
            ],
            "vectors": {"valid": 15, "invalid": 12},
        },
    },
    {
        "id": "authenticated_text_replay_response_session_fixtures",
        "status": "implemented",
        "owner": "pinned_repeater_room_authenticated_text_orchestration_rules",
        "semantic": True,
        "scope": (
            "canonical_repeater_room_authenticated_text_admin_role_newer_"
            "equal_older_session_handler_post_ack_response_creation_outcome_"
            "and_stored_path_dispatch_projection_only_no_decryption_text_"
            "parse_hash_handler_storage_packet_pool_dispatch_timing_route_"
            "execution_identity_secret_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-AUTHENTICATED-TEXT-ORCHESTRATION-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp",
                "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp",
                "third_party/MeshCore/src/helpers/ClientACL.h",
                "third_party/MeshCore/src/helpers/TxtDataHelpers.h",
            ],
            "vectors": {"valid": 20, "invalid": 17},
        },
    },
    {
        "id": "login_response_creation_dispatch_orchestration_fixtures",
        "status": "implemented",
        "owner": "pinned_repeater_room_login_response_dispatch_rules",
        "semantic": True,
        "scope": (
            "canonical_repeater_room_ready_or_absent_login_response_encoded_"
            "path_creation_kind_secret_source_exact_room_push_and_response_"
            "delays_creation_outcome_and_coarse_dispatch_projection_only_"
            "flood_transport_scope_required_no_packet_pool_path_copy_clock_"
            "queue_dispatch_execution_storage_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-LOGIN-RESPONSE-DISPATCH-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp",
                "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp",
                "third_party/MeshCore/src/Mesh.cpp",
                "third_party/MeshCore/src/Packet.cpp",
            ],
            "vectors": {"valid": 17, "invalid": 14},
        },
    },
    {
        "id": "signed_advert_dispatch_transition_fixtures",
        "status": "implemented",
        "owner": "pinned_mesh_signed_advert_gate_and_route_rules",
        "semantic": True,
        "scope": (
            "canonical_post_decode_and_flood_filter_pass_complete_self_seen_"
            "gate_caller_supplied_"
            "identity_result_callback_and_flood_route_capacity_projection_"
            "with_direct_nonzero_intercept_and_d1l_overlong_and_path_count_"
            "wrap_reject_only_"
            "upstream_identity_parity_false_no_external_verifier_contact_"
            "table_path_queue_dispatch_execution_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-SIGNED-ADVERT-DISPATCH-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/src/Mesh.cpp",
                "third_party/MeshCore/src/Identity.cpp",
                "third_party/MeshCore/src/Packet.cpp",
            ],
            "upstream_identity_parity_proven": False,
            "d1l_overlong_reject_diverges_from_upstream_truncation": True,
            "d1l_path_count_wrap_reject_diverges_from_upstream": True,
            "vectors": {"valid": 12, "invalid": 19},
        },
    },
    {
        "id": "signed_advert_creation_send_orchestration_fixtures",
        "status": "implemented",
        "owner": "pinned_mesh_dispatcher_advert_send_ordering_rules",
        "semantic": True,
        "scope": (
            "createadvert_length_pool_error_rtc_vendored_sign_then_flood_"
            "transport_direct_zero_hop_route_seen_release_retain_queue_order_"
            "projection_only_no_keypair_consistency_self_verify_packet_pool_"
            "clock_table_path_copy_queue_dispatch_execution_or_rf"
        ),
        "implementation_receipt": {
            "id": "RCPT-WP04-SIGNED-ADVERT-SEND-20260713",
            "status": "implemented",
            "observed_at": "2026-07-13",
            "pinned_sources": [
                "third_party/MeshCore/src/Mesh.cpp",
                "third_party/MeshCore/src/Dispatcher.cpp",
                "third_party/MeshCore/src/Identity.cpp",
                "third_party/MeshCore/src/Packet.cpp",
                "third_party/MeshCore/lib/ed25519/sign.c",
            ],
            "signature_self_verified": False,
            "keypair_consistency_proven": False,
            "vectors": {"valid": 16, "invalid": 9},
        },
    },
]
EXPECTED_ORACLE_REQUIRED_SURFACES = [
    {
        "id": "identity_signed_adverts",
        "status": "partial",
        "capabilities": [
            "advert_data_fields",
            "signed_advert_packet_creation",
            "signed_advert_verification",
            "ed25519_point_validation",
            "signed_advert_dispatch_transition_fixtures",
            "signed_advert_creation_send_orchestration_fixtures",
            "identity_signed_advert",
        ],
    },
    {
        "id": "public_group_packets",
        "status": "implemented",
        "capabilities": ["public_group_packets"],
    },
    {
        "id": "dm_encrypt_decrypt",
        "status": "implemented",
        "capabilities": ["dm_encrypt_decrypt"],
    },
    {
        "id": "expected_ack",
        "status": "implemented",
        "capabilities": ["expected_ack_hash_and_ack_path"],
    },
    {
        "id": "ack_multi_ack_ack_path",
        "status": "implemented",
        "capabilities": ["ack_frames", "expected_ack_hash_and_ack_path"],
    },
    {
        "id": "direct_flood_headers",
        "status": "implemented",
        "capabilities": ["direct_flood_headers"],
    },
    {
        "id": "path_return_route_codes",
        "status": "implemented",
        "capabilities": ["path_return_route_codes"],
    },
    {
        "id": "trace_path_discovery",
        "status": "partial",
        "capabilities": [
            "trace_source_frames",
            "trace_forwarding_and_path_discovery",
        ],
    },
    {
        "id": "login_request_response_admin",
        "status": "partial",
        "capabilities": [
            "anonymous_login_request_packets",
            "regular_request_response_packets",
            "identity_shared_secret_derivation",
            "canonical_login_response_packets",
            "login_password_authorization_fixtures",
            "existing_acl_blank_login_reuse_fixtures",
            "authorized_login_acl_transition_fixtures",
            "authenticated_request_replay_transition_fixtures",
            "authenticated_text_replay_response_session_fixtures",
            "login_response_creation_dispatch_orchestration_fixtures",
            "login_request_response_admin",
        ],
    },
]
EXPECTED_ORACLE_CROSS_CUTTING_CAPABILITIES = [
    {"id": "packet_envelope", "status": "implemented"},
    {"id": "ack_dispatch_correlation_and_delivery", "status": "implemented"},
    {"id": "route_selection_and_forwarding", "status": "blocked"},
]
EXPECTED_ORACLE_PACKET_TYPES = [
    {
        "symbol": "D1L_MESHCORE_PAYLOAD_TEXT",
        "code": 2,
        "wire_vector": "TXT",
        "wire_vector_covered": True,
        "semantic_capabilities": ["dm_encrypt_decrypt"],
    },
    {
        "symbol": "D1L_MESHCORE_PAYLOAD_ACK",
        "code": 3,
        "wire_vector": "ACK",
        "wire_vector_covered": True,
        "semantic_capabilities": [
            "ack_frames",
            "expected_ack_hash_and_ack_path",
            "ack_dispatch_correlation_and_delivery",
        ],
    },
    {
        "symbol": "D1L_MESHCORE_PAYLOAD_ADVERT",
        "code": 4,
        "wire_vector": "ADVERT",
        "wire_vector_covered": True,
        "semantic_capabilities": [
            "advert_data_fields",
            "signed_advert_packet_creation",
            "signed_advert_verification",
            "ed25519_point_validation",
            "signed_advert_dispatch_transition_fixtures",
            "signed_advert_creation_send_orchestration_fixtures",
            "identity_signed_advert",
        ],
    },
    {
        "symbol": "D1L_MESHCORE_PAYLOAD_GROUP_TEXT",
        "code": 5,
        "wire_vector": "GRP_TXT",
        "wire_vector_covered": True,
        "semantic_capabilities": ["public_group_packets"],
    },
    {
        "symbol": "D1L_MESHCORE_PAYLOAD_PATH",
        "code": 8,
        "wire_vector": "PATH",
        "wire_vector_covered": True,
        "semantic_capabilities": [
            "expected_ack_hash_and_ack_path",
            "path_return_route_codes",
        ],
    },
    {
        "symbol": "D1L_MESHCORE_PAYLOAD_TRACE",
        "code": 9,
        "wire_vector": "TRACE",
        "wire_vector_covered": True,
        "semantic_capabilities": [
            "trace_source_frames",
            "trace_forwarding_and_path_discovery",
        ],
    },
    {
        "symbol": "D1L_MESHCORE_PAYLOAD_MULTIPART",
        "code": 10,
        "wire_vector": "MULTIPART",
        "wire_vector_covered": True,
        "semantic_capabilities": [
            "ack_frames",
            "ack_dispatch_correlation_and_delivery",
        ],
    },
]
EXPECTED_ORACLE_UPSTREAM_SOURCE_PATHS = {
    "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp",
    "third_party/MeshCore/examples/simple_repeater/MyMesh.h",
    "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp",
    "third_party/MeshCore/src/Dispatcher.cpp",
    "third_party/MeshCore/src/Dispatcher.h",
    "third_party/MeshCore/src/Identity.cpp",
    "third_party/MeshCore/src/Identity.h",
    "third_party/MeshCore/src/Mesh.h",
    "third_party/MeshCore/src/Mesh.cpp",
    "third_party/MeshCore/src/MeshCore.h",
    "third_party/MeshCore/src/Packet.cpp",
    "third_party/MeshCore/src/Packet.h",
    "third_party/MeshCore/src/Utils.cpp",
    "third_party/MeshCore/src/Utils.h",
    "third_party/MeshCore/src/helpers/BaseChatMesh.cpp",
    "third_party/MeshCore/src/helpers/BaseChatMesh.h",
    "third_party/MeshCore/src/helpers/ClientACL.cpp",
    "third_party/MeshCore/src/helpers/ClientACL.h",
    "third_party/MeshCore/src/helpers/CommonCLI.h",
    "third_party/MeshCore/src/helpers/TxtDataHelpers.h",
    "third_party/MeshCore/src/helpers/AdvertDataHelpers.cpp",
    "third_party/MeshCore/src/helpers/AdvertDataHelpers.h",
    "third_party/MeshCore/lib/ed25519/ed_25519.h",
    "third_party/MeshCore/lib/ed25519/fe.c",
    "third_party/MeshCore/lib/ed25519/fe.h",
    "third_party/MeshCore/lib/ed25519/fixedint.h",
    "third_party/MeshCore/lib/ed25519/ge.c",
    "third_party/MeshCore/lib/ed25519/ge.h",
    "third_party/MeshCore/lib/ed25519/key_exchange.c",
    "third_party/MeshCore/lib/ed25519/keypair.c",
    "third_party/MeshCore/lib/ed25519/license.txt",
    "third_party/MeshCore/lib/ed25519/precomp_data.h",
    "third_party/MeshCore/lib/ed25519/sc.c",
    "third_party/MeshCore/lib/ed25519/sc.h",
    "third_party/MeshCore/lib/ed25519/sha512.c",
    "third_party/MeshCore/lib/ed25519/sha512.h",
    "third_party/MeshCore/lib/ed25519/sign.c",
    "third_party/MeshCore/lib/ed25519/verify.c",
}
EXPECTED_ORACLE_PRODUCTION_BINDING_SOURCE_PATHS = {
    "main/CMakeLists.txt",
    "main/comms/usb_command_parser.c",
    "main/comms/usb_command_parser.h",
    "main/comms/usb_console.c",
    "main/mesh/advert_data.h",
    "main/mesh/contact_store.c",
    "main/mesh/contact_store.h",
    "main/mesh/dm_delivery_state.c",
    "main/mesh/dm_delivery_state.h",
    "main/mesh/dm_store.c",
    "main/mesh/dm_store.h",
    "main/mesh/ed25519_canonical.h",
    "main/mesh/meshcore_admin_dispatch.c",
    "main/mesh/meshcore_admin_dispatch.h",
    "main/mesh/meshcore_admin_runtime.c",
    "main/mesh/meshcore_admin_runtime.h",
    "main/mesh/meshcore_ack_completion.h",
    "main/mesh/meshcore_command_guard.h",
    "main/mesh/meshcore_crypto.c",
    "main/mesh/meshcore_crypto.h",
    "main/mesh/meshcore_identity_exchange.c",
    "main/mesh/meshcore_identity_exchange.h",
    "main/mesh/meshcore_dm_retry.h",
    "main/mesh/meshcore_discovery.h",
    "main/mesh/meshcore_lifetime.h",
    "main/mesh/meshcore_packet_hash.c",
    "main/mesh/meshcore_packet_hash.h",
    "main/mesh/meshcore_path_dispatch.h",
    "main/mesh/meshcore_path_state.c",
    "main/mesh/meshcore_path_state.h",
    "main/mesh/meshcore_packet_semantics.c",
    "main/mesh/meshcore_packet_semantics.h",
    "main/mesh/meshcore_route_selection.h",
    "main/mesh/meshcore_runtime_guard.h",
    "main/mesh/meshcore_service.c",
    "main/mesh/meshcore_service.h",
    "main/mesh/meshcore_text_plaintext.c",
    "main/mesh/meshcore_text_plaintext.h",
    "main/mesh/meshcore_trace.h",
    "main/mesh/meshcore_wire.h",
    "main/mesh/node_store.c",
    "main/mesh/node_store.h",
    "main/mesh/user_text.c",
    "main/mesh/user_text.h",
    "overlays/meshcore_ed25519_defined/fe.c",
    "overlays/meshcore_ed25519_defined/ge.c",
    "overlays/meshcore_ed25519_defined/license.txt",
    "overlays/meshcore_ed25519_defined/sc.c",
}
EXPECTED_ORACLE_VENDORED_CRYPTO_SOURCE_PATHS = {
    "third_party/sensecap_indicator_esp32/components/LoRaWAN/soft-se/aes.c",
    "third_party/sensecap_indicator_esp32/components/LoRaWAN/soft-se/aes.h",
}
HOST_ADMIN_NEGATIVE_MATRIX = [
    "wrong_password",
    "wrong_full_key_colliding_prefix",
    "wrong_secret_mac",
    "malformed_payload",
    "truncated_payload",
    "extra_payload",
    "unsolicited_response",
    "wrong_tag_response",
    "duplicate_response",
    "late_response",
    "replayed_response",
    "replayed_request",
    "stale_replayed_login_timestamp",
    "permission_failure",
    "response_from_another_full_identity",
    "request_from_another_full_identity",
]
HOST_ADMIN_REPEATER_STATS_FIELD_ORDER = [
    "batt_milli_volts",
    "curr_tx_queue_len",
    "noise_floor",
    "last_rssi",
    "n_packets_recv",
    "n_packets_sent",
    "total_air_time_secs",
    "total_up_time_secs",
    "n_sent_flood",
    "n_sent_direct",
    "n_recv_flood",
    "n_recv_direct",
    "err_events",
    "last_snr",
    "n_direct_dups",
    "n_flood_dups",
    "total_rx_air_time_secs",
    "n_recv_errors",
]
EXPECTED_HOST_ADMIN_REPEATER_STATS = {
    "batt_milli_volts": 3700,
    "curr_tx_queue_len": 7,
    "noise_floor": -117,
    "last_rssi": -83,
    "n_packets_recv": 0x01020304,
    "n_packets_sent": 0x11121314,
    "total_air_time_secs": 0x21222324,
    "total_up_time_secs": 0x31323334,
    "n_sent_flood": 0x41424344,
    "n_sent_direct": 0x51525354,
    "n_recv_flood": 0x61626364,
    "n_recv_direct": 0x71727374,
    "err_events": 0x8182,
    "last_snr": -20,
    "n_direct_dups": 0x9192,
    "n_flood_dups": 0xA1A2,
    "total_rx_air_time_secs": 0xB1B2B3B4,
    "n_recv_errors": 0xC1C2C3C4,
}
HOST_ADMIN_GET_STATUS_RESPONSE_HEX = (
    "05030201740e07008bffadff0403020114131211242322213433323144434241"
    "5453525164636261747372718281ecff9291a2a1b4b3b2b1c4c3c2c1"
)
EXPECTED_HOST_ADMIN_SESSION_FIXTURE = {
    "schema_version": 1,
    "receipt_id": "RCPT-WP04-HOST-ADMIN-SESSION-20260714",
    "status": "implemented",
    "host_only": True,
    "production_runtime_proven": False,
    "wp18_closure_eligible": False,
    "rf_closure_eligible": False,
    "ui_closure_eligible": False,
    "hardware_closure_eligible": False,
    "source": "tests/meshcore_oracle/meshcore_admin_session_fixture.cpp",
    "receipt_field": "admin_session_fixture",
    "identity_bytes": 32,
    "login_payload_type": "ANON_REQ",
    "login_response_bytes": 13,
    "request_schema": (
        "LE32 unique tag, GET_STATUS 0x01, four reserved zero bytes, four "
        "deterministic uniqueness bytes"
    ),
    "response_schema": (
        "LE32 reflected request tag followed by the explicit little-endian "
        "56-byte RepeaterStats fields in simple_repeater/MyMesh.h order"
    ),
    "get_status_response_bytes": 60,
    "repeater_stats_bytes": 56,
    "repeater_stats_field_order": HOST_ADMIN_REPEATER_STATS_FIELD_ORDER,
    "response_correlation": "exact LE32 tag and complete 32-byte server identity",
    "routes": ["direct", "flood_path_return"],
    "lifecycle": [
        "timeout_pending_login_state_zeroization",
        "timeout_pending_request_state_zeroization",
        "explicit_logout_client_server_session_zeroization",
    ],
    "positive_checks": 6,
    "negative_checks": 16,
    "negative_matrix": HOST_ADMIN_NEGATIVE_MATRIX,
    "pinned_upstream_sources": [
        "third_party/MeshCore/src/helpers/BaseChatMesh.cpp",
        "third_party/MeshCore/src/helpers/ClientACL.cpp",
        "third_party/MeshCore/src/helpers/ClientACL.h",
        "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp",
        "third_party/MeshCore/examples/simple_repeater/MyMesh.h",
        "third_party/MeshCore/src/Mesh.cpp",
        "third_party/MeshCore/src/Packet.cpp",
        "third_party/MeshCore/src/Utils.cpp",
    ],
}
EXPECTED_HOST_ONLY_EVIDENCE = {
    "id": "RCPT-WP04-HOST-ADMIN-SESSION-20260714",
    "status": "implemented",
    "receipt_field": "admin_session_fixture",
    "host_only": True,
    "production_runtime_proven": False,
    "wp18_closure_eligible": False,
    "rf_closure_eligible": False,
    "ui_closure_eligible": False,
    "hardware_closure_eligible": False,
    "full_identity_bytes": 32,
    "prefix_authorization": False,
    "positive_checks": 6,
    "negative_checks": 16,
    "remaining_surface_status": (
        "partial_until_concrete_packet_pool_dispatch_runtime_and_physical_"
        "delivery_evidence"
    ),
}


class GateFailure(RuntimeError):
    """A fail-closed gate condition with a concise artifact-safe message."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_lf_text_file(path: Path) -> str:
    """Hash tracked text using the repository's canonical LF representation."""
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def run_process(
    command: Sequence[str],
    *,
    cwd: Path = ROOT,
    timeout: int = 300,
    env: dict[str, str] | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(
            list(command),
            cwd=cwd,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            env=env,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        raise GateFailure(f"command could not complete: {command[0]}: {exc}") from exc
    if check and result.returncode != 0:
        stderr = result.stderr[-12_000:].strip()
        stdout = result.stdout[-4_000:].strip()
        detail = stderr or stdout or "no process output"
        raise GateFailure(
            f"command failed with exit {result.returncode}: {command[0]}: {detail}"
        )
    return result


def git_output(*args: str) -> str:
    return run_process(["git", *args], timeout=30).stdout.strip()


def load_manifest() -> dict[str, Any]:
    try:
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateFailure(f"cannot read conformance manifest: {exc}") from exc
    if manifest.get("schema_version") != 1:
        raise GateFailure("manifest schema version drifted")
    if manifest.get("coverage_boundary") != "wire_envelope_only":
        raise GateFailure("manifest coverage boundary is not wire_envelope_only")
    if manifest.get("issue_65_closure_eligible") is not False:
        raise GateFailure("manifest must keep Issue #65 closure eligibility false")
    if manifest.get("upstream") != EXPECTED_UPSTREAM:
        raise GateFailure("manifest upstream pin or source allowlist drifted")
    if manifest.get("production_wire_sources") != EXPECTED_PRODUCTION_WIRE_SOURCES:
        raise GateFailure("manifest production wire source list drifted")
    if manifest.get("fuzz_corpus") != EXPECTED_FUZZ_CORPUS:
        raise GateFailure("manifest fuzz corpus pin drifted")
    if manifest.get("advert_fuzz_corpus") != EXPECTED_ADVERT_FUZZ_CORPUS:
        raise GateFailure("manifest advert fuzz corpus pin drifted")
    if manifest.get("constants") != {
        "max_raw_packet": 255,
        "max_path_bytes": 64,
        "max_packet_payload": 184,
    }:
        raise GateFailure("manifest wire constants drifted")
    if manifest.get("vectors") != EXPECTED_VECTORS:
        raise GateFailure("manifest vector counts drifted")
    if manifest.get("payload_version_gate") != EXPECTED_PAYLOAD_VERSION_GATE:
        raise GateFailure("manifest payload version gate drifted")
    if manifest.get("vector_matrix") != EXPECTED_VECTOR_MATRIX:
        raise GateFailure("manifest vector matrix drifted")
    if manifest.get("fuzz") != EXPECTED_FUZZ:
        raise GateFailure("manifest fuzz contract drifted")
    if manifest.get("advert_fuzz") != EXPECTED_ADVERT_FUZZ:
        raise GateFailure("manifest advert fuzz contract drifted")
    return manifest


def load_oracle_manifest() -> dict[str, Any]:
    try:
        manifest = json.loads(ORACLE_MANIFEST_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateFailure(f"cannot read MeshCore oracle manifest: {exc}") from exc
    if manifest.get("schema_version") != 1:
        raise GateFailure("oracle manifest schema version drifted")
    if manifest.get("corpus_version") != ORACLE_CORPUS_VERSION:
        raise GateFailure("oracle corpus version drifted")
    if manifest.get("abi_version") != ORACLE_ABI_VERSION:
        raise GateFailure("oracle ABI version drifted")
    if manifest.get("coverage_boundary") != ORACLE_COVERAGE_BOUNDARY:
        raise GateFailure("oracle coverage boundary drifted")
    if manifest.get("wp04_closure_eligible") is not False:
        raise GateFailure("oracle manifest must not claim WP-04 closure")
    if manifest.get("sanitizer_policy") != ED25519_SANITIZER_POLICY:
        raise GateFailure("oracle sanitizer exception policy drifted")
    upstream = manifest.get("upstream")
    if not isinstance(upstream, dict) or upstream.get("commit") != EXPECTED_UPSTREAM["commit"]:
        raise GateFailure("oracle upstream commit does not match the pinned MeshCore pin")
    if upstream.get("path") != EXPECTED_UPSTREAM["path"]:
        raise GateFailure("oracle upstream path drifted")
    expected_upstream_sources = {
        f"{EXPECTED_UPSTREAM['path']}/{relative}": digest
        for relative, digest in EXPECTED_UPSTREAM["sources"].items()
    }
    upstream_sources = upstream.get("sources")
    if not isinstance(upstream_sources, dict):
        raise GateFailure("oracle upstream source allowlist is missing")
    if set(upstream_sources) != EXPECTED_ORACLE_UPSTREAM_SOURCE_PATHS:
        raise GateFailure("oracle upstream source allowlist drifted")
    for relative, digest in expected_upstream_sources.items():
        if upstream_sources.get(relative) != digest:
            raise GateFailure(f"oracle core upstream source pin drifted: {relative}")
    production_binding_sources = manifest.get("production_binding_sources")
    if (
        not isinstance(production_binding_sources, dict)
        or set(production_binding_sources)
        != EXPECTED_ORACLE_PRODUCTION_BINDING_SOURCE_PATHS
    ):
        raise GateFailure("oracle production-binding source allowlist drifted")
    vendored_crypto_sources = manifest.get("vendored_crypto_sources")
    if (
        not isinstance(vendored_crypto_sources, dict)
        or set(vendored_crypto_sources)
        != EXPECTED_ORACLE_VENDORED_CRYPTO_SOURCE_PATHS
    ):
        raise GateFailure("oracle vendored-crypto source allowlist drifted")
    if manifest.get("capabilities") != EXPECTED_ORACLE_CAPABILITIES:
        raise GateFailure("oracle capability registry drifted")
    if manifest.get("host_admin_session_fixture") != (
        EXPECTED_HOST_ADMIN_SESSION_FIXTURE
    ):
        raise GateFailure("host admin-session fixture contract drifted")
    if manifest.get("vectors") != {
        "roundtrip": 338,
        "valid": 144,
        "invalid": 449,
        "semantic": 915,
        "total": 931,
        "packet_envelope": {
            "roundtrip": 4,
            "invalid": 5,
            "semantic": 0,
            "total": 9,
        },
        "advert_data_fields": {
            "roundtrip": 4,
            "invalid": 11,
            "semantic": 15,
            "total": 15,
        },
        "signed_advert_packet_creation": {
            "roundtrip": 3,
            "invalid": 23,
            "semantic": 26,
            "total": 26,
        },
        "signed_advert_verification": {
            "valid": 3,
            "invalid": 11,
            "semantic": 14,
            "total": 14,
        },
        "ed25519_verifier_kat": {
            "valid": 1,
            "invalid": 3,
            "semantic": 0,
            "total": 4,
        },
        "ed25519_point_validation": {
            "valid": 4,
            "invalid": 7,
            "semantic": 11,
            "total": 11,
        },
        "crypto_adapter_kat": {
            "valid": 3,
            "invalid": 0,
            "semantic": 0,
            "total": 3,
        },
        "identity_shared_secret_derivation": {
            "roundtrip": 5,
            "invalid": 9,
            "semantic": 14,
            "total": 14,
        },
        "public_group_packets": {
            "roundtrip": 4,
            "invalid": 21,
            "semantic": 25,
            "total": 25,
        },
        "anonymous_login_request_packets": {
            "roundtrip": 6,
            "invalid": 35,
            "semantic": 41,
            "total": 41,
        },
        "regular_request_response_packets": {
            "roundtrip": 6,
            "invalid": 30,
            "semantic": 36,
            "total": 36,
        },
        "canonical_login_response_packets": {
            "roundtrip": 10,
            "invalid": 34,
            "semantic": 44,
            "total": 44,
        },
        "login_password_authorization_fixtures": {
            "valid": 16,
            "invalid": 17,
            "semantic": 33,
            "total": 33,
        },
        "existing_acl_blank_login_reuse_fixtures": {
            "valid": 12,
            "invalid": 14,
            "semantic": 26,
            "total": 26,
        },
        "authorized_login_acl_transition_fixtures": {
            "valid": 16,
            "invalid": 13,
            "semantic": 29,
            "total": 29,
        },
        "authenticated_request_replay_transition_fixtures": {
            "valid": 15,
            "invalid": 12,
            "semantic": 27,
            "total": 27,
        },
        "authenticated_text_replay_response_session_fixtures": {
            "valid": 20,
            "invalid": 17,
            "semantic": 37,
            "total": 37,
        },
        "login_response_creation_dispatch_orchestration_fixtures": {
            "valid": 17,
            "invalid": 14,
            "semantic": 31,
            "total": 31,
        },
        "signed_advert_dispatch_transition_fixtures": {
            "valid": 12,
            "invalid": 19,
            "semantic": 31,
            "total": 31,
        },
        "signed_advert_creation_send_orchestration_fixtures": {
            "valid": 16,
            "invalid": 9,
            "semantic": 25,
            "total": 25,
        },
        "dm_encrypt_decrypt": {
            "roundtrip": 268,
            "invalid": 29,
            "semantic": 297,
            "total": 297,
        },
        "expected_ack_hash_and_ack_path": {
            "roundtrip": 4,
            "valid": 9,
            "invalid": 35,
            "semantic": 48,
            "total": 48,
        },
        "path_return_route_codes": {
            "roundtrip": 6,
            "invalid": 35,
            "semantic": 41,
            "total": 41,
        },
        "direct_flood_headers": {
            "roundtrip": 7,
            "invalid": 10,
            "semantic": 17,
            "total": 17,
        },
        "ack_frames": {
            "roundtrip": 5,
            "invalid": 17,
            "semantic": 22,
            "total": 22,
        },
        "trace_source_frames": {
            "roundtrip": 6,
            "invalid": 19,
            "semantic": 25,
            "total": 25,
        },
    }:
        raise GateFailure("oracle vector contract drifted")
    interface = manifest.get("interface", {})
    if (
        interface.get("language") != "c_abi"
        or interface.get("upstream_types")
        != ["mesh::Packet", "mesh::Utils", "AdvertDataBuilder", "AdvertDataParser"]
        or interface.get("reject_preserves_output") is not True
        or interface.get("crypto_available") is not False
        or interface.get("group_crypto_available") is not True
        or interface.get("group_crypto_scope")
        != "aes128_ecb_zero_padding_truncated_hmac_sha256_and_basechatmesh_setchannel_hash_only"
        or interface.get("dm_crypto_available") is not True
        or interface.get("dm_crypto_scope")
        != "basechatmesh_plain_text_layout_and_mesh_datagram_crypto_with_caller_supplied_hashes_and_shared_secret_only"
        or interface.get("expected_ack_path_available") is not True
        or interface.get("expected_ack_path_scope")
        != "plain_dm_expected_hash_all_lengths_defined_ack_bodies_and_authenticated_ack_path_with_caller_supplied_identity_hash_secret_and_rng_inputs_only"
        or interface.get("path_return_route_codes_available") is not True
        or interface.get("path_return_route_codes_scope")
        != "general_authenticated_path_return_extra_and_no_extra_uniqueness_framing_with_caller_supplied_hash_secret_rng_and_route_inputs_only_no_route_selection_dispatch_forwarding_or_rf"
        or interface.get("signed_advert_ed25519_available") is not True
        or interface.get("signed_advert_scope")
        != "d1l_production_message_layout_strict_points_and_ed25519_verification_only_no_mesh_dispatch"
        or interface.get("signed_advert_packet_creation_available") is not True
        or interface.get("signed_advert_packet_creation_scope")
        != "deterministic_seed_keypair_pre_route_creation_and_authenticated_parse_only_no_identity_verifier_dispatch_replay_contact_or_rf"
        or interface.get("anonymous_login_request_available") is not True
        or interface.get("anonymous_login_request_scope")
        != "canonical_room_nonroom_login_request_create_authenticated_parse_with_caller_supplied_hash_public_key_secret_time_and_room_mode_only_no_key_exchange_authorization_replay_response_session_dispatch_route_or_rf"
        or interface.get("regular_request_response_available") is not True
        or interface.get("regular_request_response_scope")
        != "authenticated_nonempty_req_response_create_parse_with_caller_supplied_type_hashes_secret_and_logical_length_only_no_schema_tag_authorization_replay_session_dispatch_route_or_rf"
        or interface.get("identity_shared_secret_derivation_available") is not True
        or interface.get("identity_shared_secret_derivation_scope")
        != "valid_input_seed_keypair_strict_peer_edwards_to_montgomery_shared_secret_with_zero_rejection_only_no_persisted_key_contact_auth_session_dispatch_route_production_integration_or_rf"
        or interface.get("canonical_login_response_available") is not True
        or interface.get("canonical_login_response_scope")
        != "repeater_room_success_response_schema_authenticated_packet_with_caller_supplied_hashes_secret_time_permissions_and_uniqueness_only_no_password_acl_replay_session_transition_dispatch_route_or_rf"
        or interface.get("login_password_authorization_available") is not True
        or interface.get("login_password_authorization_scope")
        != "repeater_room_acl_miss_password_admin_guest_read_only_authorization_and_denial_only_no_existing_acl_contact_mutation_replay_secret_session_response_dispatch_route_or_rf"
        or interface.get("existing_acl_blank_login_reuse_available") is not True
        or interface.get("existing_acl_blank_login_reuse_scope")
        != "repeater_room_blank_password_full_public_key_first_match_acl_reuse_response_secret_selection_and_client_out_path_mutation_boundary_only_no_storage_load_save_insert_evict_permissions_secret_timestamp_activity_replay_session_response_creation_dispatch_route_or_rf"
        or interface.get("authorized_login_acl_transition_available") is not True
        or interface.get("authorized_login_acl_transition_scope")
        != "clientacl_putclient_full_key_reuse_append_least_active_non_admin_evict_and_repeater_room_authorized_login_record_transition_projection_only_no_filesystem_full_path_bytes_unmodified_room_scheduling_identity_signature_secret_derivation_password_response_dispatch_route_or_rf"
        or interface.get("authenticated_request_replay_transition_available")
        is not True
        or interface.get("authenticated_request_replay_transition_scope")
        != "repeater_room_prevalidated_authenticated_request_timestamp_duplicate_handler_result_keep_alive_and_record_session_transition_projection_only_no_request_schema_handler_execution_response_hash_creation_storage_identity_secret_dispatch_route_schedule_or_rf"
        or interface.get("authenticated_text_transition_available") is not True
        or interface.get("authenticated_text_transition_scope")
        != "repeater_room_canonical_authenticated_text_newer_equal_older_role_handler_post_session_ack_response_creation_outcome_and_stored_path_dispatch_projection_only_no_decryption_text_parse_hash_handler_storage_packet_pool_dispatch_timing_route_execution_identity_secret_or_rf"
        or interface.get("login_response_dispatch_transition_available") is not True
        or interface.get("login_response_dispatch_transition_scope")
        != "repeater_room_canonical_response_ready_encoded_path_creation_kind_secret_source_exact_2000ms_room_push_and_300ms_coarse_dispatch_projection_only_flood_transport_scope_required_no_packet_pool_path_copy_clock_queue_dispatch_execution_storage_or_rf"
        or interface.get("signed_advert_dispatch_transition_available") is not True
        or interface.get("signed_advert_dispatch_transition_scope")
        != "mesh_post_decode_and_flood_filter_pass_complete_self_seen_caller_supplied_identity_result_callback_and_canonical_flood_route_capacity_projection_with_direct_nonzero_intercept_and_d1l_overlong_and_path_count_wrap_reject_only_upstream_identity_parity_false_no_external_verifier_tables_path_copy_clock_queue_dispatch_or_rf"
        or interface.get("signed_advert_send_transition_available") is not True
        or interface.get("signed_advert_send_transition_scope")
        != "mesh_createadvert_length_pool_error_rtc_sign_then_flood_transport_direct_zero_hop_ownership_queue_order_projection_only_signature_bytes_covered_separately_no_keypair_consistency_self_verify_packet_pool_clock_table_path_copy_queue_dispatch_or_rf"
        or interface.get("canonical_advert_data") is not True
        or interface.get("route_header_scope")
        != "non_trace_direct_flood_and_zero_hop_headers"
        or interface.get("ack_frame_scope")
        != "simple_and_multipart_payload_framing_only"
        or interface.get("trace_frame_scope")
        != "initial_outbound_direct_flags_zero_trace_framing_only"
        or interface.get("golden_vector_sources")
        != {
            "direct_flood_headers": "third_party/MeshCore/src/Mesh.cpp",
            "ack_frames": "third_party/MeshCore/src/Mesh.cpp",
            "trace_source_frames": "third_party/MeshCore/src/Mesh.cpp",
            "signed_advert_message_layout": "third_party/MeshCore/src/Mesh.cpp",
            "signed_advert_fixed_seed_keypair": (
                "third_party/MeshCore/lib/ed25519/keypair.c"
            ),
            "signed_advert_fixed_seed_signer": (
                "third_party/MeshCore/lib/ed25519/sign.c"
            ),
            "signed_advert_verifier": (
                "third_party/MeshCore/lib/ed25519/verify.c"
            ),
            "signed_advert_point_decoder": (
                "overlays/meshcore_ed25519_defined/ge.c"
            ),
                "signed_advert_independent_kat": "RFC 8032 section 7.1 TEST 1",
                "anonymous_login_plaintext": (
                    "third_party/MeshCore/src/helpers/BaseChatMesh.cpp"
                ),
                "anonymous_login_datagram": "third_party/MeshCore/src/Mesh.cpp",
                "anonymous_login_crypto": "third_party/MeshCore/src/Utils.cpp",
                "anonymous_login_independent_golden": (
                    "two exact payloads and six-vector matrix from independent "
                    "AES-128 ECB and HMAC-SHA-256 generation"
                ),
                "regular_request_response_datagram": (
                    "third_party/MeshCore/src/Mesh.cpp"
                ),
                "regular_request_response_crypto": (
                    "third_party/MeshCore/src/Utils.cpp"
                ),
                "regular_request_response_independent_golden": (
                    "two exact payloads plus maximum-payload and six-vector "
                    "matrix SHA-256 from independent AES-128 ECB and "
                    "HMAC-SHA-256 generation"
                ),
                "identity_exchange_keypair": (
                    "third_party/MeshCore/lib/ed25519/keypair.c"
                ),
                "identity_exchange_ladder": (
                    "third_party/MeshCore/lib/ed25519/key_exchange.c"
                ),
                "identity_exchange_strict_peer_guard": (
                    "main/mesh/ed25519_canonical.h"
                ),
                "identity_exchange_independent_golden": (
                    "five symmetric public-key and shared-secret vectors "
                    "independently generated through libsodium Ed25519-to-"
                    "Curve25519 conversion and scalar multiplication"
                ),
                "canonical_login_response_repeater_schema": (
                    "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp"
                ),
                "canonical_login_response_room_schema": (
                    "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp"
                ),
                "canonical_login_response_datagram": (
                    "third_party/MeshCore/src/Mesh.cpp"
                ),
                "canonical_login_response_crypto": (
                    "third_party/MeshCore/src/Utils.cpp"
                ),
                "canonical_login_response_permissions": (
                    "third_party/MeshCore/src/helpers/ClientACL.h"
                ),
                "canonical_login_response_independent_golden": (
                    "two exact payloads and ten-vector matrix SHA-256 from "
                    "independent AES-128 ECB and HMAC-SHA-256 generation"
                ),
                "login_password_authorization_repeater": (
                    "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp"
                ),
                "login_password_authorization_room": (
                    "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp"
                ),
                "login_password_authorization_permissions": (
                    "third_party/MeshCore/src/helpers/ClientACL.h"
                ),
                "login_password_authorization_preferences": (
                    "third_party/MeshCore/src/helpers/CommonCLI.h"
                ),
                "existing_acl_lookup": (
                    "third_party/MeshCore/src/helpers/ClientACL.cpp"
                ),
                "existing_acl_client_record": (
                    "third_party/MeshCore/src/helpers/ClientACL.h"
                ),
                "existing_acl_repeater_reuse": (
                    "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp"
                ),
                "existing_acl_room_reuse": (
                    "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp"
                ),
                "authorized_login_acl_put_client": (
                    "third_party/MeshCore/src/helpers/ClientACL.cpp"
                ),
                "authorized_login_acl_record": (
                    "third_party/MeshCore/src/helpers/ClientACL.h"
                ),
                "authorized_login_acl_repeater_transition": (
                    "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp"
                ),
                "authorized_login_acl_room_transition": (
                    "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp"
                ),
                "authenticated_request_replay_repeater": (
                    "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp"
                ),
                "authenticated_request_replay_room": (
                    "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp"
                ),
                "authenticated_request_replay_record": (
                    "third_party/MeshCore/src/helpers/ClientACL.h"
                ),
                "authenticated_text_repeater": (
                    "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp"
                ),
                "authenticated_text_room": (
                    "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp"
                ),
                "authenticated_text_record": (
                    "third_party/MeshCore/src/helpers/ClientACL.h"
                ),
                "authenticated_text_types": (
                    "third_party/MeshCore/src/helpers/TxtDataHelpers.h"
                ),
                "login_response_dispatch_repeater": (
                    "third_party/MeshCore/examples/simple_repeater/MyMesh.cpp"
                ),
                "login_response_dispatch_room": (
                    "third_party/MeshCore/examples/simple_room_server/MyMesh.cpp"
                ),
                "login_response_dispatch_mesh_creation": (
                    "third_party/MeshCore/src/Mesh.cpp"
                ),
                "signed_advert_dispatch_mesh": (
                    "third_party/MeshCore/src/Mesh.cpp"
                ),
                "signed_advert_dispatch_identity_boundary": (
                    "third_party/MeshCore/src/Identity.cpp"
                ),
                "signed_advert_dispatch_route": (
                    "third_party/MeshCore/src/Mesh.cpp"
                ),
                "signed_advert_send_mesh": "third_party/MeshCore/src/Mesh.cpp",
                "signed_advert_send_dispatcher": (
                    "third_party/MeshCore/src/Dispatcher.cpp"
                ),
                "signed_advert_send_identity": (
                    "third_party/MeshCore/src/Identity.cpp"
                ),
                "signed_advert_send_signer": (
                    "third_party/MeshCore/lib/ed25519/sign.c"
                ),
            "public_group_channel_hash": (
                "third_party/MeshCore/src/helpers/BaseChatMesh.cpp"
            ),
            "public_group_datagram": "third_party/MeshCore/src/Mesh.cpp",
            "public_group_encrypt_mac_parse": "third_party/MeshCore/src/Utils.cpp",
            "public_group_aes": (
                "third_party/sensecap_indicator_esp32/components/LoRaWAN/soft-se/aes.c"
            ),
            "public_group_sha_hmac": "tests/meshcore_oracle/stubs/SHA256.h",
            "public_group_independent_kats": (
                "FIPS 180-4 SHA-256 abc; RFC 4231 HMAC test 1; "
                "FIPS 197 AES-128 cipher example"
            ),
            "dm_datagram": "third_party/MeshCore/src/Mesh.cpp",
            "dm_plain_text_layout": (
                "third_party/MeshCore/src/helpers/BaseChatMesh.cpp"
            ),
            "dm_encrypt_mac_parse": "third_party/MeshCore/src/Utils.cpp",
            "dm_independent_golden": (
                "attempt 0 and 255 exact payloads; attempt matrix and maximum "
                "plus exact-block payload SHA-256 digests"
            ),
            "expected_ack_hash": (
                "third_party/MeshCore/src/helpers/BaseChatMesh.cpp"
            ),
            "ack_path_datagram": "third_party/MeshCore/src/Mesh.cpp",
            "ack_path_encrypt_mac_parse": "third_party/MeshCore/src/Utils.cpp",
            "ack_path_independent_golden": (
                "four exact ACK bodies; exact-block expected hash; zero-path "
                "exact payload; four-path payload matrix SHA-256"
            ),
            "path_return_datagram": "third_party/MeshCore/src/Mesh.cpp",
            "path_return_encrypt_mac_parse": "third_party/MeshCore/src/Utils.cpp",
            "path_return_route_codes": "third_party/MeshCore/src/Mesh.cpp",
            "path_return_independent_golden": (
                "zero-path exact payload; general extra and no-extra uniqueness "
                "matrix SHA-256; direct flood and transport-code route matrix"
            ),
        }
    ):
        raise GateFailure("oracle interface boundary drifted")
    determinism = manifest.get("determinism", {})
    if determinism.get("current_vectors") != (
        "fixed_bytes_ed25519_signed_advert_packets_identity_exchange_public_"
        "group_anonymous_login_regular_request_response_canonical_login_"
        "response_login_password_authorization_existing_acl_blank_login_"
        "reuse_authorized_login_acl_transition_authenticated_request_replay_"
        "transition_authenticated_text_replay_response_session_"
        "orchestration_login_response_creation_dispatch_orchestration_"
        "signed_advert_dispatch_transition_signed_advert_creation_send_"
        "orchestration_dm_ack_path_and_"
        "general_path_return_vectors_no_runtime_rng_or_clock"
    ):
        raise GateFailure("oracle deterministic vector source drifted")
    if determinism.get("signed_advert_seed_hex") != (
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f"
    ):
        raise GateFailure("oracle signed-advert seed provenance drifted")
    if determinism.get("signed_advert_generation_recipe") != (
        "ed25519_create_keypair_seed_then_ed25519_sign_"
        "public_key_timestamp_wire_bytes_app_data"
    ):
        raise GateFailure("oracle signed-advert generation recipe drifted")
    if determinism.get("anonymous_login_recipe") != (
        "basechatmesh_timestamp_optional_room_sync_password_then_mesh_"
        "anonymous_datagram_aes128_hmac_with_caller_supplied_outer_and_secret"
    ):
        raise GateFailure("oracle anonymous-login recipe drifted")
    if determinism.get("anonymous_login_matrix") != (
        "nonroom_empty_exact12_max15_and_room_empty_exact8_max15"
    ):
        raise GateFailure("oracle anonymous-login matrix drifted")
    if determinism.get("regular_request_response_recipe") != (
        "mesh_req_response_hash_prefix_then_utils_aes128_ecb_zero_pad_and_two_"
        "byte_hmac_sha256_with_caller_logical_length"
    ):
        raise GateFailure("oracle request/response recipe drifted")
    if determinism.get("regular_request_response_matrix") != (
        "request_lengths_1_15_16_17_and_response_lengths_16_167"
    ):
        raise GateFailure("oracle request/response matrix drifted")
    if determinism.get("identity_exchange_recipe") != (
        "ed25519_seed_keypair_strict_peer_y_to_montgomery_ladder_zero_secret_"
        "reject_and_temp_wipe"
    ):
        raise GateFailure("oracle identity-exchange recipe drifted")
    if determinism.get("identity_exchange_matrix") != (
        "five_symmetric_seed_pairs_including_all_zero_seed_independently_"
        "generated_with_libsodium"
    ):
        raise GateFailure("oracle identity-exchange matrix drifted")
    if determinism.get("canonical_login_response_recipe") != (
        "repeater_or_room_success_schema_then_mesh_response_datagram_aes128_"
        "hmac_with_caller_supplied_hashes_secret_time_permissions_and_"
        "uniqueness"
    ):
        raise GateFailure("oracle canonical login-response recipe drifted")
    if determinism.get("canonical_login_response_matrix") != (
        "repeater_and_room_guest_read_only_read_write_admin_and_flagged_role_"
        "permissions"
    ):
        raise GateFailure("oracle canonical login-response matrix drifted")
    if determinism.get("login_password_authorization_recipe") != (
        "repeater_room_acl_miss_admin_first_guest_second_optional_room_read_"
        "only_fallback"
    ):
        raise GateFailure("oracle login password authorization recipe drifted")
    if determinism.get("login_password_authorization_matrix") != (
        "sixteen_allow_deny_precedence_empty_maximum_and_role_cases"
    ):
        raise GateFailure("oracle login password authorization matrix drifted")
    if determinism.get("existing_acl_blank_login_recipe") != (
        "blank_password_full_public_key_first_match_then_server_secret_source_"
        "and_flood_out_path_mutation_classification"
    ):
        raise GateFailure("oracle existing ACL blank-login recipe drifted")
    if determinism.get("existing_acl_blank_login_matrix") != (
        "twelve_empty_first_middle_last_duplicate_prefix_full_capacity_match_"
        "miss_route_and_server_cases"
    ):
        raise GateFailure("oracle existing ACL blank-login matrix drifted")
    if determinism.get("authorized_login_acl_transition_recipe") != (
        "clientacl_putclient_full_key_reuse_append_or_strict_least_active_non_"
        "admin_evict_then_replay_gate_and_server_record_field_updates"
    ):
        raise GateFailure("oracle authorized login ACL transition recipe drifted")
    if determinism.get("authorized_login_acl_transition_matrix") != (
        "sixteen_append_existing_replay_capacity_eviction_tie_all_admin_role_"
        "secret_time_room_state_dirty_and_flood_cases"
    ):
        raise GateFailure("oracle authorized login ACL transition matrix drifted")
    if determinism.get("authenticated_request_replay_transition_recipe") != (
        "prevalidated_request_repeater_strict_newer_and_handler_commit_vs_"
        "room_equal_accept_prehandler_commit_and_direct_keep_alive_session_"
        "update"
    ):
        raise GateFailure("oracle authenticated-request replay recipe drifted")
    if determinism.get("authenticated_request_replay_transition_matrix") != (
        "fifteen_repeater_room_newer_equal_older_handler_success_failure_keep_"
        "alive_force_since_path_and_timestamp_boundary_cases"
    ):
        raise GateFailure("oracle authenticated-request replay matrix drifted")
    if determinism.get("authenticated_text_transition_recipe") != (
        "canonical_text_repeater_admin_gate_and_room_role_gate_then_equal_"
        "retry_session_commit_handler_or_post_suppression_creation_outcome_"
        "and_stored_path_dispatch_projection"
    ):
        raise GateFailure("oracle authenticated-text recipe drifted")
    if determinism.get("authenticated_text_transition_matrix") != (
        "twenty_repeater_room_plain_cli_newer_equal_older_role_empty_max_"
        "timestamp_creation_failure_ack_multiplicity_and_path_cases"
    ):
        raise GateFailure("oracle authenticated-text matrix drifted")
    if determinism.get("login_response_dispatch_recipe") != (
        "ready_canonical_response_repeater_caller_secret_room_stored_secret_"
        "flood_path_return_or_direct_datagram_then_creation_outcome_exact_"
        "delays_and_coarse_dispatch_selection"
    ):
        raise GateFailure("oracle login-response dispatch recipe drifted")
    if determinism.get("login_response_dispatch_matrix") != (
        "seventeen_absent_repeater_room_flood_direct_encoded_path_creation_"
        "success_failure_room_push_delay_and_unknown_path_cases"
    ):
        raise GateFailure("oracle login-response dispatch matrix drifted")
    if determinism.get("signed_advert_dispatch_recipe") != (
        "mesh_post_decode_and_flood_filter_pass_complete_self_seen_gate_then_"
        "caller_supplied_"
        "identity_result_callback_and_canonical_flood_route_capacity_forward_"
        "policy_projection"
    ):
        raise GateFailure("oracle signed-advert dispatch recipe drifted")
    if determinism.get("signed_advert_dispatch_matrix") != (
        "twelve_valid_invalid_self_seen_incomplete_direct_zero_hop_flood_do_"
        "not_retransmit_forward_policy_path_capacity_and_one_byte_count_wrap_"
        "reject_cases_plus_nineteen_"
        "fail_closed_boundaries"
    ):
        raise GateFailure("oracle signed-advert dispatch matrix drifted")
    if determinism.get("signed_advert_send_recipe") != (
        "createadvert_length_then_pool_error_or_rtc_and_vendored_sign_then_"
        "caller_selected_flood_transport_flood_direct_or_zero_hop_ownership_"
        "and_queue_projection"
    ):
        raise GateFailure("oracle signed-advert send recipe drifted")
    if determinism.get("signed_advert_send_matrix") != (
        "sixteen_oversize_pool_empty_flood_transport_direct_zero_hop_valid_"
        "encoded_path_invalid_route_retention_release_and_queue_cases"
    ):
        raise GateFailure("oracle signed-advert send matrix drifted")
    if determinism.get("independent_verifier_kat") != (
        "RFC 8032 section 7.1 TEST 1 empty message"
    ):
        raise GateFailure("oracle independent verifier KAT provenance drifted")
    if determinism.get("canonical_s_regression") != (
        "RFC 8032 TEST 1 signature scalar plus Ed25519 group order L"
    ):
        raise GateFailure("oracle canonical-S regression provenance drifted")
    if determinism.get("identity_point_forgery_regression") != (
        "identity public key plus identity R and zero S accepted by the pinned verifier before strict point validation"
    ):
        raise GateFailure("oracle identity-point regression provenance drifted")
    if determinism.get("public_group_recipe") != (
        "basechatmesh_setchannel_padded16_or_full32_secret_sha256_hash_then_"
        "utils_aes128_ecb_zero_pad_and_two_byte_hmac_sha256"
    ):
        raise GateFailure("oracle public-group vector provenance drifted")
    if determinism.get("dm_recipe") != (
        "basechatmesh_timestamp_low_attempt_text_optional_full_attempt_then_"
        "mesh_hash_prefix_and_utils_aes128_hmac"
    ):
        raise GateFailure("oracle DM vector provenance drifted")
    if determinism.get("dm_attempt_matrix") != "0_through_255":
        raise GateFailure("oracle DM attempt matrix drifted")
    if determinism.get("dm_exact_block_matrix") != (
        "normal_text_lengths_11_through_155_step_16"
    ):
        raise GateFailure("oracle exact-block DM matrix drifted")
    if determinism.get("expected_ack_recipe") != (
        "basechatmesh_timestamp_low_attempt_text_sender_public_key_sha256_"
        "truncated4_plus_optional_full_attempt_and_caller_rng_byte"
    ):
        raise GateFailure("oracle expected-ACK provenance drifted")
    if determinism.get("ack_path_recipe") != (
        "mesh_path_return_encoded_path_ack_extra_then_utils_aes128_hmac"
    ):
        raise GateFailure("oracle ACK+PATH provenance drifted")
    if determinism.get("ack_path_encodings") != [
        "zero",
        "one_byte",
        "two_byte",
        "three_byte",
    ]:
        raise GateFailure("oracle ACK+PATH encoding matrix drifted")
    if determinism.get("path_return_recipe") != (
        "mesh_createpathreturn_general_extra_or_caller_rng_uniqueness_then_"
        "utils_aes128_hmac"
    ):
        raise GateFailure("oracle PATH-return provenance drifted")
    if determinism.get("path_return_encodings") != [
        "zero",
        "one_byte",
        "two_byte_maximum",
        "three_byte_maximum",
    ]:
        raise GateFailure("oracle PATH-return encoding matrix drifted")
    if determinism.get("path_return_route_matrix") != (
        "flood_transport_flood_direct_zero_hop_and_transport_zero_hop"
    ):
        raise GateFailure("oracle PATH-return route matrix drifted")
    if determinism.get("undefined_full_ack_body_boundary") != (
        "normal_dm_5_plus_text_len_exact_aes_block_expected_hash_only"
    ):
        raise GateFailure("oracle ACK boundary receipt drifted")
    required_fixtures = determinism.get(
        "future_fixtures_required"
    )
    if required_fixtures != [
        "mock_radio",
        "deterministic_rng",
        "deterministic_rtc",
        "deterministic_millisecond_clock",
        "packet_manager",
        "mesh_tables",
        "contacts",
        "channels",
    ]:
        raise GateFailure("oracle deterministic fixture roadmap drifted")
    oracle_sources = manifest.get("oracle_sources")
    if not isinstance(oracle_sources, dict) or set(oracle_sources) != {
        "tests/meshcore_oracle/coverage_manifest.json",
        "tests/meshcore_oracle/meshcore_oracle.cpp",
        "tests/meshcore_oracle/meshcore_oracle.h",
        "tests/meshcore_oracle/meshcore_admin_session_fixture.cpp",
        "tests/meshcore_oracle/meshcore_admin_session_fixture.h",
        "tests/meshcore_oracle/meshcore_oracle_vectors.cpp",
        "tests/meshcore_oracle/wp05_semantic_matrix.json",
        "tests/meshcore_oracle/stubs/AES.h",
        "tests/meshcore_oracle/stubs/Arduino.h",
        "tests/meshcore_oracle/stubs/SHA256.h",
        "tests/meshcore_oracle/stubs/Stream.h",
    }:
        raise GateFailure("oracle source allowlist drifted")
    return manifest


def load_wp05_semantic_matrix(
    oracle_manifest: dict[str, Any],
    *,
    matrix_path: Path = WP05_SEMANTIC_MATRIX_PATH,
) -> dict[str, Any]:
    try:
        matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateFailure(f"cannot read WP-05 semantic matrix: {exc}") from exc
    if matrix.get("schema_version") != 1:
        raise GateFailure("WP-05 semantic matrix schema version drifted")
    if matrix.get("work_package") != "WP-05" or matrix.get("matrix_version") != 1:
        raise GateFailure("WP-05 semantic matrix identity drifted")
    if matrix.get("upstream_commit") != EXPECTED_UPSTREAM["commit"]:
        raise GateFailure("WP-05 semantic matrix upstream pin drifted")
    if matrix.get("coverage_boundary") != "declared_wp05_host_semantic_matrix":
        raise GateFailure("WP-05 semantic matrix boundary drifted")
    if matrix.get("closure_ready") is not False:
        raise GateFailure("WP-05 semantic matrix must remain fail closed")

    oracle_vectors = oracle_manifest.get("vectors")
    if not isinstance(oracle_vectors, dict):
        raise GateFailure("WP-05 semantic matrix cannot resolve oracle vectors")
    vector_groups = {
        key for key, value in oracle_vectors.items() if isinstance(value, dict)
    }

    requirements = matrix.get("requirements")
    if not isinstance(requirements, list) or [
        item.get("id") if isinstance(item, dict) else None for item in requirements
    ] != list(EXPECTED_WP05_REQUIREMENT_STATUSES):
        raise GateFailure("WP-05 semantic requirement registry drifted")
    for requirement in requirements:
        requirement_id = requirement["id"]
        status = requirement.get("status")
        if status != EXPECTED_WP05_REQUIREMENT_STATUSES[requirement_id]:
            raise GateFailure(f"WP-05 semantic requirement status drifted: {requirement_id}")
        groups = requirement.get("oracle_vector_groups")
        if (
            not isinstance(groups, list)
            or len(groups) != len(set(groups))
            or any(group not in vector_groups for group in groups)
        ):
            raise GateFailure(f"WP-05 oracle vector mapping is invalid: {requirement_id}")
        suites = requirement.get("production_host_suites", [])
        if (
            not isinstance(suites, list)
            or len(suites) != len(set(suites))
            or any(suite not in EXPECTED_WP05_PRODUCTION_SUITES for suite in suites)
        ):
            raise GateFailure(f"WP-05 production-suite mapping is invalid: {requirement_id}")
        companion_suites = requirement.get("companion_upstream_suites", [])
        if (
            not isinstance(companion_suites, list)
            or len(companion_suites) != len(set(companion_suites))
            or any(
                suite not in EXPECTED_WP05_COMPANION_UPSTREAM_SUITES
                for suite in companion_suites
            )
        ):
            raise GateFailure(
                f"WP-05 companion-suite mapping is invalid: {requirement_id}"
            )
        remaining = requirement.get("remaining")
        if (
            not isinstance(remaining, list)
            or any(not isinstance(item, str) or not item for item in remaining)
            or (status == "implemented" and remaining)
            or (status != "implemented" and not remaining)
        ):
            raise GateFailure(f"WP-05 remaining-gap receipt drifted: {requirement_id}")
        if status == "missing" and (groups or suites or companion_suites):
            raise GateFailure(f"WP-05 missing requirement claims evidence: {requirement_id}")
        if status != "missing" and not groups and not suites and not companion_suites:
            raise GateFailure(
                f"WP-05 non-missing requirement lacks evidence: {requirement_id}"
            )

    fuzz_targets = matrix.get("fuzz_targets")
    if not isinstance(fuzz_targets, list) or [
        item.get("id") if isinstance(item, dict) else None for item in fuzz_targets
    ] != list(EXPECTED_WP05_FUZZ_TARGET_STATUSES):
        raise GateFailure("WP-05 fuzz-target registry drifted")
    for target in fuzz_targets:
        if target.get("status") != EXPECTED_WP05_FUZZ_TARGET_STATUSES[target["id"]]:
            raise GateFailure(f"WP-05 fuzz-target status drifted: {target['id']}")

    suites = matrix.get("production_host_suites")
    if not isinstance(suites, list) or [
        item.get("id") if isinstance(item, dict) else None for item in suites
    ] != list(EXPECTED_WP05_PRODUCTION_SUITES):
        raise GateFailure("WP-05 production host-suite registry drifted")
    scenario_ids: set[str] = set()
    for suite in suites:
        contract = EXPECTED_WP05_PRODUCTION_SUITES[suite["id"]]
        scenarios = suite.get("scenario_ids")
        invocations = suite.get("invocations")
        if (
            suite.get("binary") != contract["binary"]
            or suite.get("expected_stdout") != contract["expected_stdout"]
            or not isinstance(scenarios, list)
            or len(scenarios) != contract["scenario_count"]
            or len(scenarios) != len(set(scenarios))
            or any(not isinstance(item, str) or not item for item in scenarios)
            or scenario_ids.intersection(scenarios)
            or not isinstance(invocations, list)
            or len(invocations) != contract["invocation_count"]
            or any(
                not isinstance(arguments, list)
                or any(not isinstance(argument, str) for argument in arguments)
                for arguments in invocations
            )
        ):
            raise GateFailure(f"WP-05 production host-suite contract drifted: {suite['id']}")
        scenario_ids.update(scenarios)

    companion_suites = matrix.get("companion_upstream_suites")
    if not isinstance(companion_suites, list) or [
        item.get("id") if isinstance(item, dict) else None
        for item in companion_suites
    ] != list(EXPECTED_WP05_COMPANION_UPSTREAM_SUITES):
        raise GateFailure("WP-05 companion upstream-suite registry drifted")
    companion_case_ids: set[str] = set()
    for suite in companion_suites:
        contract = EXPECTED_WP05_COMPANION_UPSTREAM_SUITES[suite["id"]]
        case_ids = suite.get("case_ids")
        if (
            suite.get("artifact_type") != contract["artifact_type"]
            or suite.get("capability") != contract["capability"]
            or case_ids != contract["case_ids"]
            or len(case_ids) != len(set(case_ids))
            or companion_case_ids.intersection(case_ids)
            or suite.get("identical_wire_hash_case")
            != contract["identical_wire_hash_case"]
            or suite.get("identical_wire_hash_case") in case_ids
            or suite.get("closure_ready") is not False
        ):
            raise GateFailure(
                f"WP-05 companion upstream-suite contract drifted: {suite['id']}"
            )
        companion_case_ids.update(case_ids)

    source_pins = matrix.get("source_pins")
    if not isinstance(source_pins, dict) or set(source_pins) != EXPECTED_WP05_SOURCE_PATHS:
        raise GateFailure("WP-05 production semantic source allowlist drifted")
    for relative, digest in source_pins.items():
        if (
            "\\" in relative
            or not isinstance(digest, str)
            or re.fullmatch(r"[0-9a-f]{64}", digest) is None
        ):
            raise GateFailure("WP-05 production semantic source pin is invalid")
    if re.search(r"\bCOM(?:8|11|29)\b", json.dumps(matrix), re.IGNORECASE):
        raise GateFailure("WP-05 semantic matrix contains a forbidden port")
    return matrix


def verify_wp05_semantic_sources(matrix: dict[str, Any]) -> dict[str, Any]:
    failures: list[str] = []
    files: dict[str, Any] = {}
    root = ROOT.resolve()
    for relative, expected_hash in matrix["source_pins"].items():
        source = (ROOT / relative).resolve()
        try:
            source.relative_to(root)
        except ValueError as exc:
            raise GateFailure(f"WP-05 semantic source escapes repository root: {relative}") from exc
        actual_hash = sha256_lf_text_file(source) if source.is_file() else None
        matched = actual_hash == expected_hash
        files[relative] = {
            "expected_sha256": expected_hash,
            "actual_sha256": actual_hash,
            "matched": matched,
        }
        if not matched:
            failures.append(f"WP-05 semantic source hash mismatch: {relative}")
    result = {
        "verified": False,
        "pins_verified": not failures,
        "dependency_closure_verified": False,
        "dependency_verification": None,
        "source_hash_mode": "canonical_lf_text_sha256",
        "files": files,
        "failures": failures,
    }
    if failures:
        raise GateFailure("; ".join(failures))
    return result


def summarize_wp05_semantic_matrix(
    matrix: dict[str, Any], oracle_manifest: dict[str, Any]
) -> dict[str, Any]:
    oracle_vectors = oracle_manifest["vectors"]
    requirements: list[dict[str, Any]] = []
    referenced_groups: set[str] = set()
    for requirement in matrix["requirements"]:
        groups = requirement["oracle_vector_groups"]
        referenced_groups.update(groups)
        evidence = {
            group: oracle_vectors[group]["semantic"] for group in groups
        }
        requirements.append(
            {
                **requirement,
                "oracle_semantic_vector_evidence": evidence,
                "referenced_semantic_vectors": sum(evidence.values()),
            }
        )
    requirement_statuses = [item["status"] for item in requirements]
    fuzz_statuses = [item["status"] for item in matrix["fuzz_targets"]]
    return {
        "path": str(WP05_SEMANTIC_MATRIX_PATH.relative_to(ROOT)).replace("\\", "/"),
        "sha256": sha256_lf_text_file(WP05_SEMANTIC_MATRIX_PATH),
        "validated": True,
        "coverage_boundary": matrix["coverage_boundary"],
        "closure_ready": False,
        "full_semantic_matrix_covered": False,
        "requirement_count": len(requirements),
        "implemented_requirement_count": requirement_statuses.count("implemented"),
        "partial_requirement_count": requirement_statuses.count("partial"),
        "missing_requirement_count": requirement_statuses.count("missing"),
        "fuzz_target_count": len(fuzz_statuses),
        "implemented_fuzz_target_count": fuzz_statuses.count("implemented"),
        "partial_fuzz_target_count": fuzz_statuses.count("partial"),
        "missing_fuzz_target_count": fuzz_statuses.count("missing"),
        "oracle_semantic_vectors": oracle_vectors["semantic"],
        "unique_referenced_oracle_semantic_vectors": sum(
            oracle_vectors[group]["semantic"] for group in referenced_groups
        ),
        "production_suite_count": len(matrix["production_host_suites"]),
        "production_scenario_count": sum(
            len(suite["scenario_ids"])
            for suite in matrix["production_host_suites"]
        ),
        "companion_upstream_suite_count": len(
            matrix["companion_upstream_suites"]
        ),
        "companion_upstream_case_count": sum(
            len(suite["case_ids"])
            for suite in matrix["companion_upstream_suites"]
        ),
        "requirements": requirements,
        "fuzz_targets": matrix["fuzz_targets"],
        "production_host_suites": matrix["production_host_suites"],
        "companion_upstream_suites": matrix["companion_upstream_suites"],
        "source_verification": None,
        "result": None,
    }


def parse_local_packet_types(
    path: Path,
    excluded_symbols: Sequence[str] = ("D1L_MESHCORE_PAYLOAD_VER_1",),
) -> dict[str, int]:
    try:
        source = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise GateFailure(f"cannot read local MeshCore packet type source: {exc}") from exc
    prefix = "D1L_MESHCORE_PAYLOAD_"
    pattern = re.compile(
        rf"^\s*#\s*define\s+({prefix}[A-Za-z0-9_]+)\b(.*)$",
        re.MULTILINE,
    )
    excluded = set(excluded_symbols)
    declarations = pattern.findall(source)
    symbols = [symbol for symbol, _encoded in declarations]
    if len(set(symbols)) != len(symbols):
        raise GateFailure("local MeshCore packet type symbols must be unique")
    if not excluded.issubset({symbol for symbol, _encoded in declarations}):
        raise GateFailure("local MeshCore packet type exclusions drifted")
    packet_types: dict[str, int] = {}
    for symbol, encoded in declarations:
        if symbol in excluded:
            continue
        if not re.fullmatch(rf"{prefix}[A-Z0-9_]+", symbol):
            raise GateFailure("local MeshCore packet type symbol syntax is unsupported")
        value = re.fullmatch(
            r"\s*(0[xX][0-9A-Fa-f]+|[0-9]+)[uU]?\s*", encoded
        )
        if value is None:
            raise GateFailure("local MeshCore packet type value syntax is unsupported")
        code = int(value.group(1), 0)
        if code < 0 or code > 0x0F:
            raise GateFailure("local MeshCore packet type code is outside the 4-bit range")
        packet_types[symbol] = code
    if not packet_types:
        raise GateFailure("local MeshCore packet type registry is empty")
    if len(set(packet_types.values())) != len(packet_types):
        raise GateFailure("local MeshCore packet type codes must be unique")
    return packet_types


def load_oracle_coverage_manifest(
    oracle_manifest: dict[str, Any],
    wire_manifest: dict[str, Any],
    *,
    packet_type_source: Path | None = None,
    coverage_manifest_path: Path = ORACLE_COVERAGE_MANIFEST_PATH,
) -> dict[str, Any]:
    try:
        coverage = json.loads(coverage_manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateFailure(f"cannot read MeshCore oracle coverage manifest: {exc}") from exc
    if coverage.get("schema_version") != 1:
        raise GateFailure("oracle coverage manifest schema version drifted")
    if coverage.get("work_package") != "WP-04":
        raise GateFailure("oracle coverage manifest work package drifted")
    if coverage.get("coverage_boundary") != ORACLE_COVERAGE_BOUNDARY:
        raise GateFailure("oracle coverage manifest boundary drifted")
    if coverage.get("upstream_commit") != EXPECTED_UPSTREAM["commit"]:
        raise GateFailure("oracle coverage manifest upstream pin drifted")
    if coverage.get("oracle_corpus_version") != ORACLE_CORPUS_VERSION:
        raise GateFailure("oracle coverage manifest corpus version drifted")
    if coverage.get("oracle_abi_version") != ORACLE_ABI_VERSION:
        raise GateFailure("oracle coverage manifest ABI version drifted")
    if coverage.get("wp04_closure_eligible") is not False:
        raise GateFailure("oracle coverage manifest must reject WP-04 closure")
    if coverage.get("closure_ready") is not False:
        raise GateFailure("oracle coverage manifest must remain fail closed")
    if coverage.get("host_only_evidence") != EXPECTED_HOST_ONLY_EVIDENCE:
        raise GateFailure("oracle host-only evidence contract drifted")

    capabilities = oracle_manifest["capabilities"]
    capability_by_id = {entry["id"]: entry for entry in capabilities}
    if len(capability_by_id) != len(capabilities):
        raise GateFailure("oracle capability IDs must be unique")

    surfaces = coverage.get("required_surfaces")
    if surfaces != EXPECTED_ORACLE_REQUIRED_SURFACES:
        raise GateFailure("oracle required-surface mapping drifted")
    referenced_capabilities: set[str] = set()
    for surface in surfaces:
        surface_capabilities = surface.get("capabilities")
        if (
            not isinstance(surface_capabilities, list)
            or not surface_capabilities
            or len(set(surface_capabilities)) != len(surface_capabilities)
            or any(item not in capability_by_id for item in surface_capabilities)
        ):
            raise GateFailure(f"oracle surface capability mapping is invalid: {surface.get('id')}")
        statuses = {
            capability_by_id[item]["status"] for item in surface_capabilities
        }
        expected_status = (
            "implemented"
            if statuses == {"implemented"}
            else "blocked"
            if statuses == {"pending"}
            else "partial"
        )
        if surface.get("status") != expected_status:
            raise GateFailure(f"oracle surface status is not derived: {surface['id']}")
        referenced_capabilities.update(surface_capabilities)

    cross_cutting = coverage.get("cross_cutting_capabilities")
    if cross_cutting != EXPECTED_ORACLE_CROSS_CUTTING_CAPABILITIES:
        raise GateFailure("oracle cross-cutting capability mapping drifted")
    for entry in cross_cutting:
        capability_id = entry["id"]
        capability = capability_by_id.get(capability_id)
        if capability is None:
            raise GateFailure(f"unknown cross-cutting capability: {capability_id}")
        expected_status = (
            "implemented" if capability["status"] == "implemented" else "blocked"
        )
        if entry.get("status") != expected_status:
            raise GateFailure(f"cross-cutting capability status drifted: {capability_id}")
        referenced_capabilities.add(capability_id)
    if referenced_capabilities != set(capability_by_id):
        raise GateFailure("coverage manifest does not account for every oracle capability")

    pending = {
        item["id"]: item
        for item in capabilities
        if item.get("status") == "pending"
    }
    if any(not isinstance(item.get("blocked_by"), str) for item in pending.values()):
        raise GateFailure("every pending oracle capability requires an exact blocker receipt")
    if coverage.get("unresolved_capabilities") != list(pending):
        raise GateFailure("oracle unresolved capability registry drifted")

    packet_policy = coverage.get("local_packet_type_policy")
    if not isinstance(packet_policy, dict):
        raise GateFailure("local packet type policy is missing")
    if (
        packet_policy.get("source") != "main/mesh/meshcore_wire.h"
        or packet_policy.get("declaration_prefix") != "D1L_MESHCORE_PAYLOAD_"
        or packet_policy.get("excluded_symbols")
        != ["D1L_MESHCORE_PAYLOAD_VER_1"]
        or packet_policy.get("unknown_packet_type_policy") != "fail_closed"
        or packet_policy.get("require_wire_vector") is not True
    ):
        raise GateFailure("local packet type policy is not fail closed")
    packet_types = packet_policy.get("packet_types")
    if packet_types != EXPECTED_ORACLE_PACKET_TYPES:
        raise GateFailure("local packet type semantic coverage mapping drifted")
    declared = parse_local_packet_types(
        packet_type_source or ROOT / packet_policy["source"],
        packet_policy["excluded_symbols"],
    )
    covered: dict[str, int] = {}
    vector_codes: dict[str, int] = {}
    for entry in packet_types:
        symbol = entry.get("symbol")
        code = entry.get("code")
        vector = entry.get("wire_vector")
        semantic_capabilities = entry.get("semantic_capabilities")
        if (
            not isinstance(symbol, str)
            or not isinstance(code, int)
            or not isinstance(vector, str)
            or entry.get("wire_vector_covered") is not True
            or not isinstance(semantic_capabilities, list)
            or not semantic_capabilities
            or any(item not in capability_by_id for item in semantic_capabilities)
        ):
            raise GateFailure("local packet type coverage entry is invalid")
        if symbol in covered or vector in vector_codes:
            raise GateFailure("local packet type coverage entries must be unique")
        covered[symbol] = code
        vector_codes[vector] = code
    if covered != declared:
        raise GateFailure("local packet type changed without a coverage vector")
    wire_vectors = {
        item["name"]: item["code"]
        for item in wire_manifest["vector_matrix"]["payload_types"]
    }
    if vector_codes != wire_vectors:
        raise GateFailure("local packet type coverage does not match the wire vector matrix")

    update_policy = coverage.get("submodule_update_policy")
    if update_policy != {
        "reviewed_upstream_commit": EXPECTED_UPSTREAM["commit"],
        "reviewed_corpus_version": ORACLE_CORPUS_VERSION,
        "requires_vector_review": True,
        "requires_corpus_version_increment": True,
        "unreviewed_update_policy": "fail_closed",
    }:
        raise GateFailure("MeshCore submodule vector-review policy drifted")
    return coverage


def summarize_oracle_coverage(
    coverage: dict[str, Any], oracle_manifest: dict[str, Any]
) -> dict[str, Any]:
    statuses = [item["status"] for item in coverage["required_surfaces"]]
    pending = [
        item for item in oracle_manifest["capabilities"] if item["status"] == "pending"
    ]
    return {
        "path": str(ORACLE_COVERAGE_MANIFEST_PATH.relative_to(ROOT)).replace(
            "\\", "/"
        ),
        "sha256": sha256_lf_text_file(ORACLE_COVERAGE_MANIFEST_PATH),
        "validated": True,
        "wp04_closure_eligible": False,
        "closure_ready": False,
        "unsupported_closure_rejected": True,
        "required_surface_count": len(statuses),
        "implemented_surface_count": statuses.count("implemented"),
        "partial_surface_count": statuses.count("partial"),
        "blocked_surface_count": statuses.count("blocked"),
        "local_packet_type_count": len(
            coverage["local_packet_type_policy"]["packet_types"]
        ),
        "wire_vector_covered_packet_type_count": len(
            coverage["local_packet_type_policy"]["packet_types"]
        ),
        "unknown_packet_type_policy": "fail_closed",
        "required_surfaces": coverage["required_surfaces"],
        "cross_cutting_capabilities": coverage["cross_cutting_capabilities"],
        "unresolved_capabilities": coverage["unresolved_capabilities"],
        "blocker_receipts": [
            {
                "capability": item["id"],
                "status": "open",
                "blocked_by": item["blocked_by"],
                **(
                    {"receipt": item["blocker_receipt"]}
                    if "blocker_receipt" in item
                    else {}
                ),
            }
            for item in pending
        ],
        "local_packet_types": coverage["local_packet_type_policy"]["packet_types"],
        "submodule_update_policy": coverage["submodule_update_policy"],
    }


def verify_oracle_sources(manifest: dict[str, Any]) -> dict[str, Any]:
    failures: list[str] = []
    files: dict[str, Any] = {}
    expected_sources = {
        **manifest["upstream"]["sources"],
        **manifest["production_binding_sources"],
        **manifest["vendored_crypto_sources"],
        **manifest["oracle_sources"],
    }
    root = ROOT.resolve()
    for relative, expected_hash in expected_sources.items():
        if not isinstance(relative, str) or "\\" in relative:
            raise GateFailure("oracle source paths must be repository-relative POSIX paths")
        source = (ROOT / relative).resolve()
        try:
            source.relative_to(root)
        except ValueError as exc:
            raise GateFailure(f"oracle source escapes repository root: {relative}") from exc
        actual_hash = sha256_lf_text_file(source) if source.is_file() else None
        matched = actual_hash == expected_hash
        files[relative] = {
            "expected_sha256": expected_hash,
            "actual_sha256": actual_hash,
            "matched": matched,
        }
        if not matched:
            failures.append(f"oracle source hash mismatch: {relative}")
    result = {
        "verified": not failures,
        "source_hash_mode": "canonical_lf_text_sha256",
        "files": files,
        "failures": failures,
    }
    if failures:
        raise GateFailure("; ".join(failures))
    return result


def load_fuzz_corpus(
    manifest: dict[str, Any],
    *,
    pin_name: str,
    corpus_path: Path,
    coverage_boundary: str,
    max_input_bytes: int,
) -> tuple[dict[str, Any], list[tuple[str, bytes, str]]]:
    corpus_pin = manifest.get(pin_name, {})
    expected_path = str(corpus_path.relative_to(ROOT)).replace("\\", "/")
    if corpus_pin.get("manifest") != expected_path:
        raise GateFailure("primary manifest points at an unexpected fuzz corpus")
    if corpus_pin.get("sha256") != sha256_lf_text_file(corpus_path):
        raise GateFailure("fuzz corpus manifest SHA256 does not match the primary manifest")
    try:
        corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateFailure(f"cannot read fuzz corpus manifest: {exc}") from exc
    if (
        corpus.get("encoding") != "hex"
        or corpus.get("coverage_boundary") != coverage_boundary
    ):
        raise GateFailure(
            f"fuzz corpus must be hex and {coverage_boundary}"
        )
    decoded: list[tuple[str, bytes, str]] = []
    names: set[str] = set()
    for entry in corpus.get("seeds", []):
        name = entry.get("name")
        encoded = entry.get("hex")
        expected_hash = entry.get("sha256")
        if not isinstance(name, str) or not re.fullmatch(r"[a-z0-9_]+", name) or name in names:
            raise GateFailure("fuzz corpus seed names must be unique safe identifiers")
        try:
            payload = bytes.fromhex(encoded)
        except (TypeError, ValueError) as exc:
            raise GateFailure(f"invalid hex corpus seed {name}") from exc
        actual_hash = hashlib.sha256(payload).hexdigest()
        if not payload or len(payload) > max_input_bytes or actual_hash != expected_hash:
            raise GateFailure(f"fuzz corpus seed {name} failed length or SHA256 validation")
        names.add(name)
        decoded.append((name, payload, actual_hash))
    if not decoded:
        raise GateFailure("fuzz corpus manifest has no seeds")
    if len(decoded) != corpus_pin.get("seed_count"):
        raise GateFailure("fuzz corpus seed count does not match the primary manifest")
    return corpus, decoded


def load_corpus(
    manifest: dict[str, Any],
) -> tuple[dict[str, Any], list[tuple[str, bytes, str]]]:
    return load_fuzz_corpus(
        manifest,
        pin_name="fuzz_corpus",
        corpus_path=CORPUS_PATH,
        coverage_boundary="local_wire_decoder_only",
        max_input_bytes=255,
    )


def load_advert_corpus(
    manifest: dict[str, Any],
) -> tuple[dict[str, Any], list[tuple[str, bytes, str]]]:
    return load_fuzz_corpus(
        manifest,
        pin_name="advert_fuzz_corpus",
        corpus_path=ADVERT_CORPUS_PATH,
        coverage_boundary="local_advert_parser",
        max_input_bytes=33,
    )


def verify_sources(manifest: dict[str, Any], expected_commit: str | None) -> dict[str, Any]:
    upstream = manifest["upstream"]
    upstream_root = ROOT / upstream["path"]
    failures: list[str] = []
    file_results: dict[str, Any] = {}
    for relative, expected_hash in upstream["sources"].items():
        source = upstream_root / relative
        actual_hash = sha256_lf_text_file(source) if source.is_file() else None
        matched = actual_hash == expected_hash
        file_results[relative] = {
            "expected_sha256": expected_hash,
            "actual_sha256": actual_hash,
            "matched": matched,
        }
        if not matched:
            failures.append(f"upstream source hash mismatch: {relative}")

    upstream_head = git_output("-C", str(upstream_root), "rev-parse", "HEAD")
    if upstream_head != upstream["commit"]:
        failures.append("checked-out MeshCore commit does not match manifest")

    gitlink_line = git_output("ls-tree", "HEAD", "--", upstream["path"])
    gitlink_match = re.match(r"(\d{6}) (\w+) ([0-9a-f]{40})\t", gitlink_line)
    gitlink_mode = gitlink_match.group(1) if gitlink_match else None
    gitlink_type = gitlink_match.group(2) if gitlink_match else None
    gitlink_commit = gitlink_match.group(3) if gitlink_match else None
    if gitlink_mode != "160000" or gitlink_type != "commit":
        failures.append("repository MeshCore entry is not a gitlink")
    if gitlink_commit != upstream["commit"]:
        failures.append("repository gitlink does not match manifest")

    repository_commit = git_output("rev-parse", "HEAD")
    if expected_commit and repository_commit != expected_commit:
        failures.append(
            f"repository commit {repository_commit} does not match --commit {expected_commit}"
        )

    result = {
        "verified": not failures,
        "repository_commit": repository_commit,
        "expected_repository_commit": expected_commit,
        "upstream_commit": upstream_head,
        "checkout_commit": repository_commit,
        "gitlink_mode": gitlink_mode,
        "gitlink_type": gitlink_type,
        "gitlink_commit": gitlink_commit,
        "expected_upstream_commit": upstream["commit"],
        "source_hash_mode": "canonical_lf_text_sha256",
        "source_files": file_results,
        "allowlisted_source_sha256_verified": all(
            item["matched"] for item in file_results.values()
        ),
        "failures": failures,
    }
    if failures:
        raise GateFailure("; ".join(failures))
    return result


def production_semantic_suite_specs(
    build_dir: str = "$BUILD_DIR",
) -> list[dict[str, Any]]:
    native = ROOT / "tests" / "native"
    common_includes = [native / "stubs", native, ROOT / "main"]
    return [
        {
            "id": "admin_dispatch",
            "binary": Path(build_dir) / "meshcore_admin_dispatch_semantic",
            "flags": [],
            "include_dirs": [ROOT / "main"],
            "sources": [
                native / "meshcore_admin_dispatch_test.c",
                ROOT / "main" / "mesh" / "meshcore_admin_dispatch.c",
            ],
        },
        {
            "id": "runtime_queue_fairness",
            "binary": Path(build_dir) / "meshcore_runtime_guard_semantic",
            "flags": [],
            "include_dirs": [ROOT / "main"],
            "sources": [
                native / "meshcore_runtime_guard_test.c",
            ],
        },
        {
            "id": "contact_trace",
            "binary": Path(build_dir) / "meshcore_trace_semantic",
            "flags": [],
            "include_dirs": [ROOT / "main"],
            "sources": [
                native / "meshcore_trace_test.c",
                ROOT / "main" / "mesh" / "meshcore_wire.c",
            ],
        },
        {
            "id": "legacy_protocol_timestamp_migration",
            "binary": Path(build_dir) / "settings_protocol_migration_semantic",
            "flags": ["-DD1L_TEST_REAL_MUTEX=1", "-pthread"],
            "include_dirs": common_includes,
            "sources": [
                ROOT / "main" / "app" / "settings_envelope.c",
                ROOT / "main" / "app" / "settings_protocol_migration.c",
                native / "esp_nvs_stubs.c",
                native / "settings_protocol_migration_test.c",
            ],
        },
        {
            "id": "time_service_migration_integration",
            "binary": Path(build_dir) / "time_service_migration_semantic",
            "flags": [
                "-DD1L_BUILD_EPOCH_SEC=1784068276ULL",
                "-Dtime=d1l_test_time",
                "-Dsettimeofday=d1l_test_settimeofday",
            ],
            "include_dirs": common_includes,
            "sources": [
                ROOT / "main" / "app" / "settings_envelope.c",
                ROOT / "main" / "app" / "settings_protocol_migration.c",
                ROOT / "main" / "app" / "settings_time_checkpoint.c",
                ROOT / "main" / "platform" / "time_service_core.c",
                ROOT / "main" / "platform" / "time_display.c",
                ROOT / "main" / "platform" / "time_service.c",
                native / "esp_nvs_stubs.c",
                native / "time_service_checkpoint_integration_test.c",
            ],
        },
        {
            "id": "text_admission",
            "binary": Path(build_dir) / "meshcore_text_plaintext_semantic",
            "flags": [],
            "include_dirs": [ROOT / "main"],
            "sources": [
                ROOT / "main" / "mesh" / "user_text.c",
                ROOT / "main" / "mesh" / "meshcore_text_plaintext.c",
                native / "meshcore_text_plaintext_test.c",
            ],
        },
        {
            "id": "ack_delivery_correlation",
            "binary": Path(build_dir) / "meshcore_ack_completion_semantic",
            "flags": [],
            "include_dirs": [ROOT / "main"],
            "sources": [
                ROOT / "main" / "mesh" / "dm_delivery_state.c",
                native / "meshcore_ack_completion_test.c",
            ],
        },
        {
            "id": "advert_replay_admission",
            "binary": Path(build_dir) / "meshcore_advert_admission_semantic",
            "flags": ["-DD1L_CONTACT_STORE_TEST_HOOKS"],
            "include_dirs": common_includes,
            "sources": [
                ROOT / "main" / "mesh" / "node_store.c",
                ROOT / "main" / "mesh" / "contact_store.c",
                ROOT / "main" / "mesh" / "meshcore_advert_admission.c",
                ROOT / "main" / "mesh" / "meshcore_packet_hash.c",
                ROOT / "main" / "mesh" / "meshcore_path_state.c",
                ROOT / "main" / "mesh" / "meshcore_wire.c",
                ROOT / "main" / "mesh" / "contact_uri.c",
                native / "esp_nvs_stubs.c",
                native / "mbedtls_md_stub.c",
                native / "store_behavior_test.c",
            ],
        },
        {
            "id": "packet_hash_and_advert_dedupe",
            "binary": Path(build_dir) / "meshcore_packet_hash_semantic",
            "flags": [],
            "include_dirs": common_includes,
            "sources": [
                ROOT / "main" / "mesh" / "meshcore_packet_hash.c",
                ROOT / "main" / "mesh" / "meshcore_wire.c",
                native / "meshcore_packet_hash_test.c",
            ],
        },
        {
            "id": "lifetime_boundaries",
            "binary": Path(build_dir) / "meshcore_lifetime_semantic",
            "flags": [],
            "include_dirs": [ROOT / "main"],
            "sources": [
                native / "meshcore_lifetime_test.c",
            ],
        },
        {
            "id": "channel_and_peer_rx_admission",
            "binary": Path(build_dir) / "meshcore_rx_admission_semantic",
            "flags": [],
            "include_dirs": common_includes,
            "sources": [
                ROOT / "main" / "mesh" / "meshcore_rx_admission.c",
                native / "meshcore_rx_admission_test.c",
            ],
        },
    ]


def _include_arguments(include_dirs: Sequence[Path]) -> list[str]:
    return [
        argument
        for include_dir in include_dirs
        for argument in ("-I", str(include_dir))
    ]


def production_semantic_common_arguments(cc: str) -> list[str]:
    return [
        cc,
        "-std=c11",
        "-O1",
        "-g",
        "-fno-omit-frame-pointer",
        "-fsanitize=address,undefined",
        "-Wall",
        "-Wextra",
        "-Werror",
    ]


def production_semantic_dependency_arguments(
    cc: str, suite_flags: Sequence[str]
) -> list[str]:
    """Keep dependency scans bound to the build's preprocessing inputs."""

    preprocessing_flags = [
        flag for flag in suite_flags if flag.startswith(("-D", "-U"))
    ]
    return [cc, "-std=c11", *preprocessing_flags]


def production_semantic_command_plan(
    cc: str, build_dir: str = "$BUILD_DIR"
) -> list[list[str]]:
    common = production_semantic_common_arguments(cc)
    return [
        [
            *common,
            *spec["flags"],
            *_include_arguments(spec["include_dirs"]),
            *[str(source) for source in spec["sources"]],
            "-o",
            str(spec["binary"]),
        ]
        for spec in production_semantic_suite_specs(build_dir)
    ]


def production_semantic_dependency_command_plan(cc: str) -> list[list[str]]:
    return [
        [
            *production_semantic_dependency_arguments(cc, spec["flags"]),
            *_include_arguments(spec["include_dirs"]),
            "-MM",
            str(source),
        ]
        for spec in production_semantic_suite_specs()
        for source in spec["sources"]
    ]


def _split_makefile_dependency_words(value: str) -> list[str]:
    """Split a make dependency payload without corrupting Windows paths."""

    words: list[str] = []
    current: list[str] = []
    index = 0
    while index < len(value):
        char = value[index]
        if char.isspace():
            if current:
                words.append("".join(current))
                current = []
            index += 1
            continue
        if (
            char == "\\"
            and index + 1 < len(value)
            and value[index + 1].isspace()
        ):
            current.append(value[index + 1])
            index += 2
            continue
        current.append(char)
        index += 1
    if current:
        words.append("".join(current))
    return words


def parse_repo_local_make_dependencies(output: str) -> list[str]:
    """Return the complete repository-local dependency set from compiler -MM."""

    logical_lines = re.sub(r"\\\r?\n", " ", output).splitlines()
    dependencies: set[str] = set()
    rule_count = 0
    root = ROOT.resolve()
    for raw_line in logical_lines:
        line = raw_line.strip()
        if not line:
            continue
        match = re.match(r"^(.+?):(?:\s+|$)(.*)$", line)
        if match is None or not match.group(1).strip():
            raise GateFailure("WP-05 compiler dependency output is malformed")
        words = _split_makefile_dependency_words(match.group(2))
        if not words:
            raise GateFailure("WP-05 compiler dependency rule is empty")
        rule_count += 1
        for word in words:
            candidate = Path(word)
            if not candidate.is_absolute():
                candidate = ROOT / candidate
            resolved = candidate.resolve()
            try:
                relative = resolved.relative_to(root).as_posix()
            except ValueError as exc:
                raise GateFailure(
                    "WP-05 compiler dependency escapes repository root: "
                    f"{word}"
                ) from exc
            if not resolved.is_file():
                raise GateFailure(
                    f"WP-05 compiler dependency is not a file: {relative}"
                )
            dependencies.add(relative)
    if rule_count == 0 or not dependencies:
        raise GateFailure("WP-05 compiler dependency output is empty")
    return sorted(dependencies)


def verify_wp05_semantic_dependencies(
    matrix: dict[str, Any],
    suite_outputs: Sequence[str],
    cc: str,
) -> dict[str, Any]:
    specs = production_semantic_suite_specs()
    expected_output_count = sum(len(spec["sources"]) for spec in specs)
    if len(suite_outputs) != expected_output_count:
        raise GateFailure("WP-05 compiler dependency translation-unit count drifted")

    suite_dependencies: list[dict[str, Any]] = []
    derived: set[str] = set()
    output_index = 0
    for spec in specs:
        dependencies: set[str] = set()
        for _source in spec["sources"]:
            dependencies.update(
                parse_repo_local_make_dependencies(suite_outputs[output_index])
            )
            output_index += 1
        ordered_dependencies = sorted(dependencies)
        derived.update(ordered_dependencies)
        suite_dependencies.append(
            {
                "id": spec["id"],
                "translation_unit_count": len(spec["sources"]),
                "dependency_count": len(ordered_dependencies),
                "dependencies": ordered_dependencies,
            }
        )

    pins = matrix["source_pins"]
    pinned = set(pins)
    unpinned = sorted(derived - pinned)
    unused = sorted(pinned - derived)
    hash_mismatches: list[str] = []
    for relative in sorted(derived & pinned):
        source = ROOT / relative
        if sha256_lf_text_file(source) != pins[relative]:
            hash_mismatches.append(relative)

    failures: list[str] = []
    if unpinned:
        failures.append(
            "WP-05 compiler dependencies are unpinned: " + ", ".join(unpinned)
        )
    if unused:
        failures.append(
            "WP-05 semantic source pins are not compiler dependencies: "
            + ", ".join(unused)
        )
    if hash_mismatches:
        failures.append(
            "WP-05 compiler dependency hash mismatch: "
            + ", ".join(hash_mismatches)
        )

    commands = [
        [_canonical_command_argument(argument) for argument in command]
        for command in production_semantic_dependency_command_plan(cc)
    ]
    return {
        "verified": not failures,
        "compiler_generated": True,
        "dependency_mode": "-MM",
        "dependency_invocation_scope": "one_translation_unit_per_command",
        "source_hash_mode": "canonical_lf_text_sha256",
        "suite_count": len(specs),
        "translation_unit_count": expected_output_count,
        "dependency_count": len(derived),
        "pinned_source_count": len(pinned),
        "dependencies": sorted(derived),
        "suite_dependencies": suite_dependencies,
        "commands": commands,
        "unpinned_dependencies": unpinned,
        "unused_pins": unused,
        "hash_mismatches": hash_mismatches,
        "failures": failures,
    }


def finalize_wp05_semantic_source_verification(
    source_verification: dict[str, Any],
    dependency_verification: dict[str, Any],
) -> None:
    source_verification["dependency_verification"] = dependency_verification
    source_verification["dependency_closure_verified"] = dependency_verification.get(
        "verified"
    ) is True
    source_verification["verified"] = bool(
        source_verification.get("pins_verified") is True
        and source_verification["dependency_closure_verified"]
    )
    source_verification["failures"] = [
        *source_verification.get("failures", []),
        *dependency_verification.get("failures", []),
    ]
    if not source_verification["verified"]:
        raise GateFailure("; ".join(source_verification["failures"]))


def command_plan(cc: str, cxx: str, build_dir: str = "$BUILD_DIR") -> list[list[str]]:
    common_sanitizers = "-fsanitize=address,undefined"
    fuzzer_compile_sanitizers = "-fsanitize=fuzzer-no-link,address,undefined"
    commands = [
        [
            cc,
            "-std=c11",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            common_sanitizers,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "main"),
            "-c",
            str(WIRE_SOURCE),
            "-o",
            str(Path(build_dir) / "meshcore_wire_vector.o"),
        ],
        [
            cxx,
            "-std=c++17",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            common_sanitizers,
            "-I",
            str(ROOT / "tests" / "meshcore_conformance" / "stubs"),
            "-isystem",
            str(ROOT / "third_party" / "MeshCore" / "src"),
            "-c",
            str(PACKET_SOURCE),
            "-o",
            str(Path(build_dir) / "meshcore_packet.o"),
        ],
        [
            cxx,
            "-std=c++17",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            common_sanitizers,
            "-I",
            str(ROOT / "tests" / "meshcore_oracle" / "stubs"),
            "-isystem",
            str(ROOT / "third_party" / "MeshCore" / "src"),
            "-c",
            str(PACKET_SOURCE),
            "-o",
            str(Path(build_dir) / "meshcore_oracle_packet.o"),
        ],
        [
            cxx,
            "-std=c++17",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            common_sanitizers,
            "-I",
            str(ROOT / "tests" / "meshcore_oracle" / "stubs"),
            "-isystem",
            str(ROOT / "third_party" / "MeshCore" / "src"),
            "-c",
            str(ADVERT_DATA_SOURCE),
            "-o",
            str(Path(build_dir) / "meshcore_advert_data.o"),
        ],
        [
            cc,
            "-std=c11",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            common_sanitizers,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D__CORTEX_M=0",
            "-DAES_DEC_PREKEYED=1",
            "-I",
            str(ORACLE_AES_ROOT),
            "-c",
            str(ORACLE_AES_SOURCE),
            "-o",
            str(Path(build_dir) / "meshcore_oracle_aes.o"),
        ],
        [
            cxx,
            "-std=c++17",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            common_sanitizers,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "tests" / "meshcore_oracle" / "stubs"),
            "-isystem",
            str(ROOT / "third_party" / "MeshCore" / "src"),
            "-c",
            str(ORACLE_UTILS_SOURCE),
            "-o",
            str(Path(build_dir) / "meshcore_oracle_utils.o"),
        ],
        [
            cxx,
            "-std=c++17",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            common_sanitizers,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "main"),
            "-I",
            str(ROOT / "tests" / "meshcore_conformance" / "stubs"),
            "-isystem",
            str(ROOT / "third_party" / "MeshCore" / "src"),
            "-I",
            str(HARNESS_PATH.parent),
            "-c",
            str(HARNESS_PATH),
            "-o",
            str(Path(build_dir) / "meshcore_conformance.o"),
        ],
        [
            cxx,
            common_sanitizers,
            str(Path(build_dir) / "meshcore_wire_vector.o"),
            str(Path(build_dir) / "meshcore_packet.o"),
            str(Path(build_dir) / "meshcore_conformance.o"),
            "-o",
            str(Path(build_dir) / "meshcore_wire_conformance"),
        ],
        [
            cxx,
            "-std=c++17",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            common_sanitizers,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ORACLE_ADAPTER_PATH.parent),
            "-I",
            str(ROOT / "main"),
            "-I",
            str(ROOT / "tests" / "meshcore_oracle" / "stubs"),
            "-I",
            str(ED25519_ROOT),
            "-isystem",
            str(ROOT / "third_party" / "MeshCore" / "src"),
            "-c",
            str(ORACLE_ADAPTER_PATH),
            "-o",
            str(Path(build_dir) / "meshcore_oracle_adapter.o"),
        ],
        [
            cxx,
            "-std=c++17",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            common_sanitizers,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ORACLE_ADAPTER_PATH.parent),
            "-I",
            str(ROOT / "main"),
            "-I",
            str(ROOT / "tests" / "meshcore_oracle" / "stubs"),
            "-I",
            str(ED25519_ROOT),
            "-isystem",
            str(ROOT / "third_party" / "MeshCore" / "src"),
            "-c",
            str(ORACLE_VECTORS_PATH),
            "-o",
            str(Path(build_dir) / "meshcore_oracle_vectors.o"),
        ],
        [
            cxx,
            "-std=c++17",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            common_sanitizers,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ORACLE_ADAPTER_PATH.parent),
            "-isystem",
            str(ROOT / "third_party" / "MeshCore" / "src"),
            "-c",
            str(ORACLE_ADMIN_SESSION_FIXTURE_PATH),
            "-o",
            str(Path(build_dir) / "meshcore_admin_session_fixture.o"),
        ],
        [
            cxx,
            common_sanitizers,
            str(Path(build_dir) / "meshcore_oracle_packet.o"),
            str(Path(build_dir) / "meshcore_advert_data.o"),
            str(Path(build_dir) / "meshcore_oracle_aes.o"),
            str(Path(build_dir) / "meshcore_oracle_utils.o"),
            str(Path(build_dir) / "meshcore_oracle_adapter.o"),
            str(Path(build_dir) / "meshcore_admin_session_fixture.o"),
            str(Path(build_dir) / "meshcore_oracle_vectors.o"),
            *[
                str(Path(build_dir) / f"meshcore_ed25519_{source.stem}.o")
                for source in ED25519_ORACLE_SOURCES
            ],
            "-o",
            str(Path(build_dir) / "meshcore_oracle_vectors"),
        ],
        [
            cc,
            "-std=c11",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            fuzzer_compile_sanitizers,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "main"),
            "-c",
            str(WIRE_SOURCE),
            "-o",
            str(Path(build_dir) / "meshcore_wire_fuzz.o"),
        ],
        [
            cxx,
            "-std=c++17",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            fuzzer_compile_sanitizers,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "main"),
            "-c",
            str(FUZZ_PATH),
            "-o",
            str(Path(build_dir) / "meshcore_fuzz.o"),
        ],
        [
            cxx,
            "-fsanitize=fuzzer,address,undefined",
            str(Path(build_dir) / "meshcore_wire_fuzz.o"),
            str(Path(build_dir) / "meshcore_fuzz.o"),
            "-o",
            str(Path(build_dir) / "meshcore_wire_fuzz"),
        ],
        [
            cc,
            "-std=c11",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            fuzzer_compile_sanitizers,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "main"),
            "-c",
            str(LOCAL_ADVERT_SOURCE),
            "-o",
            str(Path(build_dir) / "d1l_advert_data_fuzz.o"),
        ],
        [
            cxx,
            "-std=c++17",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            fuzzer_compile_sanitizers,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "main"),
            "-c",
            str(ADVERT_FUZZ_PATH),
            "-o",
            str(Path(build_dir) / "meshcore_advert_fuzz.o"),
        ],
        [
            cxx,
            "-fsanitize=fuzzer,address,undefined",
            str(Path(build_dir) / "d1l_advert_data_fuzz.o"),
            str(Path(build_dir) / "meshcore_advert_fuzz.o"),
            "-o",
            str(Path(build_dir) / "meshcore_advert_fuzz"),
        ],
    ]
    ed25519_commands = [
        [
            cc,
            "-std=c11",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            common_sanitizers,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ED25519_ROOT),
            "-c",
            str(source),
            "-o",
            str(Path(build_dir) / f"meshcore_ed25519_{source.stem}.o"),
        ]
        for source in ED25519_ORACLE_SOURCES
    ]
    return (
        commands[:5]
        + ed25519_commands
        + commands[5:]
        + production_semantic_command_plan(cc, build_dir)
    )


def _canonical_command_argument(value: str) -> str:
    """Replace run-local source, temporary, and findings paths with tokens."""

    normalized = value.replace("\\", "/")
    source_root = str(ROOT).replace("\\", "/")
    if normalized == source_root or normalized.startswith(source_root + "/"):
        normalized = "$SOURCE_ROOT" + normalized[len(source_root) :]
    else:
        for marker in ("/third_party/", "/tests/", "/main/", "/overlays/"):
            marker_index = normalized.find(marker)
            if marker_index > 0:
                normalized = "$SOURCE_ROOT" + normalized[marker_index:]
                break
        else:
            if normalized.endswith("/main"):
                normalized = "$SOURCE_ROOT/main"
    normalized = re.sub(
        r"(?:[A-Za-z]:)?(?:/[^/]+)*/d1l-meshcore-conformance-[^/]+",
        "$BUILD_DIR",
        normalized,
    )
    if normalized.startswith("-artifact_prefix="):
        normalized = "-artifact_prefix=$FINDINGS_DIR/"
    return normalized


def _signed_advert_runtime_binding_from_canonical(
    canonical: dict[str, Any],
) -> dict[str, Any]:
    canonical_bytes = json.dumps(
        canonical,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    result = canonical["result"]
    return {
        "schema_version": 1,
        "suite_id": "pinned_signed_advert_runtime",
        "artifact_type": canonical["artifact_type"],
        "evidence_profile": canonical["evidence_profile"],
        "capability": canonical["capability"],
        "repository_commit": canonical["repository"]["repository_commit"],
        "upstream_commit": canonical["repository"]["upstream_commit"],
        "canonical_sha256": hashlib.sha256(canonical_bytes).hexdigest(),
        "timestamp_replay_case_count": result["timestamp_replay_cases"],
        "timestamp_replay_rejection_count": result[
            "timestamp_replay_rejections"
        ],
        "timestamp_newer_acceptance_count": result[
            "timestamp_newer_acceptances"
        ],
        "replay_outcomes": {
            key: result[key] for key in SIGNED_ADVERT_REPLAY_OUTCOME_KEYS
        },
        "identical_wire_hash_suppression": {
            "case_id": "identical_wire_hash_suppressed",
            "suppressed": result["identical_wire_hash_suppressed"],
            "distinct_from_timestamp_replay_window": True,
        },
        "packet_hash_case_count": result["packet_hash_parity_cases"],
        "packet_hash_outcomes": {
            key: result[key] for key in SIGNED_ADVERT_PACKET_HASH_OUTCOME_KEYS
        },
        "simple_mesh_table_receipt": {
            "lookups": result["table_lookups"],
            "flood_duplicates": result["table_flood_duplicates"],
        },
        "closure_ready": False,
    }


def bind_signed_advert_runtime_receipt(
    receipt: object,
    expected_commit: str,
    *,
    receipt_is_canonical: bool = False,
) -> dict[str, Any]:
    """Validate and bind raw or packaged exact-commit signed-runtime evidence."""

    if receipt_is_canonical:
        validate_signed_advert_runtime(
            receipt,
            expected_commit,
            require_generated_at=False,
            require_commands=False,
        )
        if (
            not isinstance(receipt, dict)
            or set(receipt) != SIGNED_ADVERT_CANONICAL_FIELDS
            or receipt.get("evidence_profile") != SIGNED_ADVERT_EVIDENCE_PROFILE
        ):
            raise ValueError("signed-advert canonical evidence shape drifted")
        canonical = copy.deepcopy(receipt)
    else:
        validate_signed_advert_runtime(receipt, expected_commit)
        canonical = canonicalize_signed_advert_runtime(receipt, expected_commit)
    return _signed_advert_runtime_binding_from_canonical(canonical)


def signed_advert_runtime_binding_valid(value: Any, expected_commit: str) -> bool:
    if not isinstance(value, dict):
        return False
    outcomes = value.get("replay_outcomes")
    identical_wire = value.get("identical_wire_hash_suppression")
    packet_hash_outcomes = value.get("packet_hash_outcomes")
    simple_table = value.get("simple_mesh_table_receipt")
    return (
        set(value)
        == {
            "schema_version",
            "suite_id",
            "artifact_type",
            "evidence_profile",
            "capability",
            "repository_commit",
            "upstream_commit",
            "canonical_sha256",
            "timestamp_replay_case_count",
            "timestamp_replay_rejection_count",
            "timestamp_newer_acceptance_count",
            "replay_outcomes",
            "identical_wire_hash_suppression",
            "packet_hash_case_count",
            "packet_hash_outcomes",
            "simple_mesh_table_receipt",
            "closure_ready",
        }
        and value.get("schema_version") == 1
        and value.get("suite_id") == "pinned_signed_advert_runtime"
        and value.get("artifact_type") == SIGNED_ADVERT_ARTIFACT_TYPE
        and value.get("evidence_profile") == SIGNED_ADVERT_EVIDENCE_PROFILE
        and value.get("capability") == "identity_signed_advert_semantic_runtime"
        and value.get("repository_commit") == expected_commit
        and value.get("upstream_commit") == EXPECTED_UPSTREAM["commit"]
        and re.fullmatch(r"[0-9a-f]{64}", str(value.get("canonical_sha256") or ""))
        is not None
        and value.get("timestamp_replay_case_count") == 5
        and value.get("timestamp_replay_rejection_count") == 3
        and value.get("timestamp_newer_acceptance_count") == 2
        and isinstance(outcomes, dict)
        and list(outcomes) == list(SIGNED_ADVERT_REPLAY_OUTCOME_KEYS)
        and all(outcomes.get(key) is True for key in SIGNED_ADVERT_REPLAY_OUTCOME_KEYS)
        and identical_wire
        == {
            "case_id": "identical_wire_hash_suppressed",
            "suppressed": True,
            "distinct_from_timestamp_replay_window": True,
        }
        and value.get("packet_hash_case_count") == 5
        and isinstance(packet_hash_outcomes, dict)
        and set(packet_hash_outcomes) == set(SIGNED_ADVERT_PACKET_HASH_OUTCOME_KEYS)
        and all(
            packet_hash_outcomes.get(key) is True
            for key in SIGNED_ADVERT_PACKET_HASH_OUTCOME_KEYS
        )
        and simple_table == {"lookups": 10, "flood_duplicates": 3}
        and value.get("closure_ready") is False
    )


def validate_signed_advert_runtime_binding(
    conformance_report: object,
    signed_advert_runtime_receipt: object,
    expected_commit: str,
    *,
    receipt_is_canonical: bool = False,
) -> None:
    """Recompute and compare the signed-runtime binding, failing closed."""

    if not isinstance(conformance_report, dict):
        raise ValueError("MeshCore conformance report must be an object")
    observed = conformance_report.get("signed_advert_runtime")
    expected = bind_signed_advert_runtime_receipt(
        signed_advert_runtime_receipt,
        expected_commit,
        receipt_is_canonical=receipt_is_canonical,
    )
    if observed != expected:
        raise ValueError(
            "MeshCore conformance signed-advert runtime binding does not match "
            "the supplied exact-commit receipt"
        )


def canonicalize_release_report(report: dict[str, Any]) -> dict[str, Any]:
    """Project a live conformance receipt into deterministic package evidence.

    The live Actions artifact retains wall-clock time, elapsed time, and local
    paths for freshness and forensic review. The release package carries the
    same semantic result without those run-local values, so two independent
    runs of one source revision can produce byte-identical package payloads.
    """

    if not isinstance(report, dict):
        raise ValueError("MeshCore conformance report must be an object")
    canonical = copy.deepcopy(report)
    canonical.pop("generated_at", None)

    for field in ("commands", "fuzz_command", "advert_fuzz_command"):
        commands = canonical.get(field)
        if not isinstance(commands, list):
            continue
        normalized_commands: list[Any] = []
        for command in commands:
            if isinstance(command, list):
                normalized_commands.append(
                    [
                        _canonical_command_argument(item)
                        if isinstance(item, str)
                        else item
                        for item in command
                    ]
                )
            elif isinstance(command, str):
                normalized_commands.append(_canonical_command_argument(command))
            else:
                normalized_commands.append(command)
        canonical[field] = normalized_commands

    fuzz_result = canonical.get("fuzz_result")
    if isinstance(fuzz_result, dict):
        fuzz_result.pop("duration_ms", None)
        fuzz_result.pop("artifact_prefix", None)
    advert_fuzz_result = canonical.get("advert_fuzz_result")
    if isinstance(advert_fuzz_result, dict):
        advert_fuzz_result.pop("duration_ms", None)
        advert_fuzz_result.pop("artifact_prefix", None)

    canonical["evidence_profile"] = CANONICAL_EVIDENCE_PROFILE
    canonical["canonicalization"] = {
        "volatile_run_receipt_fields_omitted_or_normalized": list(
            VOLATILE_RUN_RECEIPT_FIELDS
        )
    }
    return canonical


def validate_completed_report(
    report: object,
    expected_commit: str,
    *,
    build_inputs_path: Path = BUILD_INPUTS_PATH,
    require_generated_at: bool = True,
    signed_advert_runtime_receipt: object | None = None,
    signed_advert_runtime_receipt_is_canonical: bool = False,
    require_signed_advert_runtime_receipt: bool = True,
) -> None:
    """Reject a top-level green receipt unless every semantic gate completed."""

    if not isinstance(report, dict):
        raise ValueError("MeshCore conformance report must be an object")
    source = report.get("source_verification")
    source = source if isinstance(source, dict) else {}
    source_files = source.get("source_files")
    source_files = source_files if isinstance(source_files, dict) else {}
    vector = report.get("vector_result")
    vector = vector if isinstance(vector, dict) else {}
    fuzz = report.get("fuzz_result")
    fuzz = fuzz if isinstance(fuzz, dict) else {}
    advert_fuzz = report.get("advert_fuzz_result")
    advert_fuzz = advert_fuzz if isinstance(advert_fuzz, dict) else {}
    corpus = report.get("corpus")
    corpus = corpus if isinstance(corpus, dict) else {}
    advert_corpus = report.get("advert_corpus")
    advert_corpus = advert_corpus if isinstance(advert_corpus, dict) else {}
    wp05_semantic_matrix = report.get("wp05_semantic_matrix")
    signed_advert_runtime = report.get("signed_advert_runtime")
    compiler = report.get("compiler")
    compiler = compiler if isinstance(compiler, dict) else {}
    scope = report.get("scope")
    scope = scope if isinstance(scope, dict) else {}
    expected_source_names = set(EXPECTED_UPSTREAM["sources"])
    source_hashes_complete = set(source_files) == expected_source_names and all(
        isinstance(item, dict)
        and item.get("matched") is True
        and item.get("expected_sha256") == EXPECTED_UPSTREAM["sources"][name]
        and item.get("actual_sha256") == EXPECTED_UPSTREAM["sources"][name]
        for name, item in source_files.items()
    )
    corpus_seeds = corpus.get("seeds")
    corpus_seeds_complete = (
        isinstance(corpus_seeds, list)
        and len(corpus_seeds) == EXPECTED_FUZZ_CORPUS["seed_count"]
        and all(
            isinstance(item, dict)
            and isinstance(item.get("name"), str)
            and isinstance(item.get("size"), int)
            and item["size"] > 0
            and re.fullmatch(r"[0-9a-f]{64}", str(item.get("sha256") or ""))
            is not None
            for item in corpus_seeds
        )
    )
    advert_corpus_seeds = advert_corpus.get("seeds")
    advert_corpus_seeds_complete = (
        isinstance(advert_corpus_seeds, list)
        and len(advert_corpus_seeds) == EXPECTED_ADVERT_FUZZ_CORPUS["seed_count"]
        and all(
            isinstance(item, dict)
            and isinstance(item.get("name"), str)
            and isinstance(item.get("size"), int)
            and item["size"] > 0
            and item["size"] <= EXPECTED_ADVERT_FUZZ["max_input_bytes"]
            and re.fullmatch(r"[0-9a-f]{64}", str(item.get("sha256") or ""))
            is not None
            for item in advert_corpus_seeds
        )
    )
    compiler_identities = (compiler.get("cc"), compiler.get("cxx"))
    commands = report.get("commands")
    commands = commands if isinstance(commands, list) else []
    toolchain = report.get("toolchain")
    toolchain = toolchain if isinstance(toolchain, dict) else {}
    clang_receipt = toolchain.get("clang")
    clang_receipt = clang_receipt if isinstance(clang_receipt, dict) else {}
    executables = clang_receipt.get("executables")
    executables = executables if isinstance(executables, dict) else {}
    cc_receipt = executables.get("cc")
    cc_receipt = cc_receipt if isinstance(cc_receipt, dict) else {}
    cxx_receipt = executables.get("cxx")
    cxx_receipt = cxx_receipt if isinstance(cxx_receipt, dict) else {}
    cc_command = cc_receipt.get("command")
    cxx_command = cxx_receipt.get("command")
    commands_are_argv = (
        len(commands) == len(command_plan("cc", "cxx"))
        and all(
            isinstance(command, list)
            and bool(command)
            and all(isinstance(argument, str) for argument in command)
            for command in commands
        )
    )
    observed_command_plan = (
        [
            json.dumps(
                [_canonical_command_argument(argument) for argument in command],
                ensure_ascii=True,
                separators=(",", ":"),
            )
            for command in commands
        ]
        if commands_are_argv
        else []
    )
    expected_command_plan: list[str] = []
    if isinstance(cc_command, str) and isinstance(cxx_command, str):
        expected_command_plan = [
            json.dumps(
                [_canonical_command_argument(argument) for argument in command],
                ensure_ascii=True,
                separators=(",", ":"),
            )
            for command in command_plan(cc_command, cxx_command, "$BUILD_DIR")
        ]
    command_plan_is_exact = (
        commands_are_argv and observed_command_plan == expected_command_plan
    )
    sanitizer_disable_flags = [
        argument
        for command in commands
        if isinstance(command, list)
        for argument in command
        if isinstance(argument, str) and argument.startswith("-fno-sanitize=")
    ]
    signed_advert_runtime_receipt_matches = False
    if signed_advert_runtime_receipt is not None:
        try:
            validate_signed_advert_runtime_binding(
                report,
                signed_advert_runtime_receipt,
                expected_commit,
                receipt_is_canonical=signed_advert_runtime_receipt_is_canonical,
            )
        except ValueError:
            pass
        else:
            signed_advert_runtime_receipt_matches = True
    required = {
        "schema_version": report.get("schema_version") == 1,
        "artifact_type": report.get("artifact_type")
        == "d1l_meshcore_wire_conformance",
        "passed": report.get("passed") is True,
        "status": report.get("status") == "pass",
        "execution_complete": report.get("execution_complete") is True,
        "coverage_boundary": report.get("coverage_boundary") == "wire_envelope_only",
        "coverage_level": report.get("coverage_level") == "wire_envelope_only",
        "semantic_coverage_level": report.get("semantic_coverage_level")
        == "partial_declared_host_semantics",
        "bounded_production_semantics": scope.get(
            "bounded_production_semantics_covered"
        )
        is True
        and scope.get("packet_semantics_covered") is False,
        "closure_ready_false": report.get("closure_ready") is False,
        "issue_65_false": report.get("issue_65_closure_eligible") is False,
        "source_verified": source.get("verified") is True,
        "source_commit": source.get("repository_commit") == expected_commit,
        "checkout_commit": source.get("checkout_commit") == expected_commit,
        "source_gitlink": source.get("gitlink_mode") == "160000"
        and source.get("gitlink_type") == "commit"
        and source.get("gitlink_commit") == EXPECTED_UPSTREAM["commit"],
        "upstream_commit": source.get("upstream_commit") == EXPECTED_UPSTREAM["commit"],
        "source_hashes": source.get("allowlisted_source_sha256_verified") is True
        and source_hashes_complete
        and source.get("failures") == [],
        "vector_matrix": report.get("vector_matrix") == EXPECTED_VECTOR_MATRIX,
        "payload_version_gate": report.get("payload_version_gate")
        == EXPECTED_PAYLOAD_VERSION_GATE,
        "corpus": corpus.get("verified") is True
        and corpus.get("path") == EXPECTED_FUZZ_CORPUS["manifest"]
        and corpus.get("manifest_sha256") == EXPECTED_FUZZ_CORPUS["sha256"]
        and corpus.get("seed_count") == EXPECTED_FUZZ_CORPUS["seed_count"]
        and corpus_seeds_complete,
        "advert_corpus": advert_corpus.get("verified") is True
        and advert_corpus.get("path") == EXPECTED_ADVERT_FUZZ_CORPUS["manifest"]
        and advert_corpus.get("manifest_sha256")
        == EXPECTED_ADVERT_FUZZ_CORPUS["sha256"]
        and advert_corpus.get("coverage_boundary") == "local_advert_parser"
        and advert_corpus.get("seed_count")
        == EXPECTED_ADVERT_FUZZ_CORPUS["seed_count"]
        and advert_corpus_seeds_complete,
        "wp05_semantic_matrix": wp05_semantic_summary_valid(
            wp05_semantic_matrix,
            expected_cc=cc_command,
        ),
        "signed_advert_runtime": signed_advert_runtime_binding_valid(
            signed_advert_runtime,
            expected_commit,
        ),
        "signed_advert_runtime_receipt_binding": (
            signed_advert_runtime_receipt_matches
            or not require_signed_advert_runtime_receipt
        ),
        "vector_passed": vector.get("passed") is True,
        "vector_counts": vector.get("vectors")
        == {"local_to_upstream": 504, "upstream_to_local": 504, "total": 1008},
        "path_counts": vector.get("path_length_encodings")
        == {"tested": 256, "valid": 119, "invalid": 137},
        "version_counts": vector.get("payload_version_gate")
        == {
            "tested": 4,
            "accepted_v1": 1,
            "rejected_future": 3,
            "preserved_rejections": 3,
        },
        "capacity_counts": vector.get("undersized_encoder_capacities") == 254,
        "vector_failures": vector.get("failures") == 0,
        "fuzz_passed": fuzz.get("passed") is True,
        "fuzz_complete": fuzz.get("engine") == "libFuzzer"
        and fuzz.get("seed") == DEFAULT_SEED
        and fuzz.get("requested_runs") == DEFAULT_RUNS
        and fuzz.get("completed_runs") == DEFAULT_RUNS
        and fuzz.get("max_input_bytes") == 255
        and fuzz.get("sanitizers") == ["address", "undefined"],
        "fuzz_findings": fuzz.get("finding_files") == [] and fuzz.get("findings") == 0,
        "advert_fuzz_passed": advert_fuzz.get("passed") is True,
        "advert_fuzz_complete": advert_fuzz.get("engine") == "libFuzzer"
        and advert_fuzz.get("target") == "local_advert_parser"
        and advert_fuzz.get("invalid_output_policy")
        == "canonical_zeroed_with_type_N"
        and advert_fuzz.get("seed") == DEFAULT_SEED
        and advert_fuzz.get("requested_runs") == DEFAULT_RUNS
        and advert_fuzz.get("completed_runs") == DEFAULT_RUNS
        and advert_fuzz.get("max_input_bytes") == 33
        and advert_fuzz.get("sanitizers") == ["address", "undefined"],
        "advert_fuzz_findings": advert_fuzz.get("finding_files") == []
        and advert_fuzz.get("findings") == 0,
        "compiler": all(
            isinstance(identity, str) and "clang version 18.1.3" in identity
            for identity in compiler_identities
        ),
        "commands": command_plan_is_exact
        and isinstance(report.get("fuzz_command"), list)
        and isinstance(report.get("advert_fuzz_command"), list),
        "sanitizer_disable_flags_absent": sanitizer_disable_flags == [],
        "sanitizer_policy": report.get("sanitizer_policy")
        == ED25519_SANITIZER_POLICY,
        "full_ubsan_clean_true": report.get("full_ubsan_clean") is True,
        "sanitizer_policy_passed": report.get("sanitizer_policy_passed") is True,
        "enabled_sanitizers_clean": report.get("sanitizer_errors") == 0
        and report.get("memory_errors") == 0
        and report.get("zero_overrun") is True
        and report.get("zero_corruption") is True,
        "failure_empty": report.get("failure") is None,
    }
    if require_generated_at:
        required["generated_at"] = isinstance(report.get("generated_at"), str)
    try:
        metadata = load_build_inputs(build_inputs_path)
        validate_clang_receipt(
            report.get("toolchain"),
            metadata,
            expected_commit,
            sha256_file(build_inputs_path),
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError):
        required["toolchain_bytes"] = False
    else:
        required["toolchain_bytes"] = True
    failed = [name for name, passed in required.items() if not passed]
    if failed:
        raise ValueError(
            "MeshCore conformance semantic validation failed: " + ", ".join(failed)
        )


def compiler_identity(compiler: str) -> str:
    result = run_process([compiler, "--version"], timeout=30)
    identity = (result.stdout or result.stderr).splitlines()[0].strip()
    if "clang" not in identity.lower():
        raise GateFailure(f"sanitizer gate requires Clang, got: {identity}")
    return identity


def parse_harness_output(stdout: str) -> dict[str, Any]:
    for line in reversed(stdout.splitlines()):
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and "vectors" in value:
            return value
    raise GateFailure("conformance harness did not emit its JSON result")


def parse_oracle_output(stdout: str) -> dict[str, Any]:
    for line in reversed(stdout.splitlines()):
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if (
            isinstance(value, dict)
            and value.get("coverage_boundary") == ORACLE_COVERAGE_BOUNDARY
        ):
            return value
    raise GateFailure("MeshCore oracle did not emit its JSON result")


def run_wp05_semantic_suites(
    matrix: dict[str, Any],
    build_dir: Path,
    *,
    timeout: int,
    env: dict[str, str],
) -> dict[str, Any]:
    suite_results: list[dict[str, Any]] = []
    completed_scenarios = 0
    for suite in matrix["production_host_suites"]:
        invocation_count = 0
        for arguments in suite["invocations"]:
            completed = run_process(
                [str(build_dir / suite["binary"]), *arguments],
                timeout=timeout,
                env=env,
            )
            if completed.stdout.strip() != suite["expected_stdout"]:
                raise GateFailure(
                    f"WP-05 semantic suite output drifted: {suite['id']}"
                )
            invocation_count += 1
        scenario_count = len(suite["scenario_ids"])
        completed_scenarios += scenario_count
        suite_results.append(
            {
                "id": suite["id"],
                "passed": True,
                "scenario_count": scenario_count,
                "invocation_count": invocation_count,
                "scenario_ids": suite["scenario_ids"],
                "expected_stdout": suite["expected_stdout"],
            }
        )
    return {
        "passed": True,
        "coverage_boundary": matrix["coverage_boundary"],
        "closure_ready": False,
        "sanitizers": ["address", "undefined"],
        "suite_count": len(suite_results),
        "scenario_count": completed_scenarios,
        "suites": suite_results,
        "failures": 0,
    }


def wp05_semantic_result_valid(
    value: Any, expected_suites: Any
) -> bool:
    if not isinstance(value, dict) or not isinstance(expected_suites, list):
        return False
    suites = value.get("suites")
    if not isinstance(suites, list) or [
        item.get("id") if isinstance(item, dict) else None for item in suites
    ] != list(EXPECTED_WP05_PRODUCTION_SUITES):
        return False
    if [
        item.get("id") if isinstance(item, dict) else None
        for item in expected_suites
    ] != list(EXPECTED_WP05_PRODUCTION_SUITES):
        return False
    for suite, declared_suite in zip(suites, expected_suites, strict=True):
        contract = EXPECTED_WP05_PRODUCTION_SUITES[suite["id"]]
        if (
            suite.get("passed") is not True
            or suite.get("scenario_count") != contract["scenario_count"]
            or suite.get("invocation_count") != contract["invocation_count"]
            or suite.get("scenario_ids") != declared_suite.get("scenario_ids")
            or suite.get("expected_stdout") != contract["expected_stdout"]
        ):
            return False
    return (
        value.get("passed") is True
        and value.get("coverage_boundary") == "declared_wp05_host_semantic_matrix"
        and value.get("closure_ready") is False
        and value.get("sanitizers") == ["address", "undefined"]
        and value.get("suite_count") == len(EXPECTED_WP05_PRODUCTION_SUITES)
        and value.get("scenario_count") == sum(
            contract["scenario_count"]
            for contract in EXPECTED_WP05_PRODUCTION_SUITES.values()
        )
        and value.get("failures") == 0
    )


def wp05_semantic_summary_valid(value: Any, *, expected_cc: Any = None) -> bool:
    if not isinstance(value, dict):
        return False
    requirements = value.get("requirements")
    fuzz_targets = value.get("fuzz_targets")
    source = value.get("source_verification")
    source = source if isinstance(source, dict) else {}
    files = source.get("files")
    files = files if isinstance(files, dict) else {}
    try:
        oracle_manifest = load_oracle_manifest()
        matrix_document = load_wp05_semantic_matrix(oracle_manifest)
        expected_summary = summarize_wp05_semantic_matrix(
            matrix_document, oracle_manifest
        )
    except (GateFailure, OSError, UnicodeError, json.JSONDecodeError, ValueError):
        return False
    static_summary_complete = all(
        value.get(key) == expected
        for key, expected in expected_summary.items()
        if key not in {"source_verification", "result"}
    )
    matrix_source_pins = matrix_document.get("source_pins")
    matrix_pins_complete = (
        isinstance(matrix_source_pins, dict)
        and set(matrix_source_pins) == EXPECTED_WP05_SOURCE_PATHS
        and all(
            isinstance(digest, str)
            and re.fullmatch(r"[0-9a-f]{64}", digest) is not None
            for digest in matrix_source_pins.values()
        )
    )
    source_complete = matrix_pins_complete and set(files) == set(
        matrix_source_pins
    ) and all(
        isinstance(item, dict)
        and item.get("matched") is True
        and item.get("expected_sha256") == matrix_source_pins[relative]
        and item.get("actual_sha256") == matrix_source_pins[relative]
        and matrix_source_pins[relative] == sha256_lf_text_file(ROOT / relative)
        and re.fullmatch(
            r"[0-9a-f]{64}", str(item.get("actual_sha256") or "")
        )
        is not None
        for relative, item in files.items()
    )
    dependency = source.get("dependency_verification")
    dependency = dependency if isinstance(dependency, dict) else {}
    dependencies = dependency.get("dependencies")
    suites = dependency.get("suite_dependencies")
    commands = dependency.get("commands")
    suite_dependency_union: set[str] = set()
    expected_suite_specs = production_semantic_suite_specs()
    suites_complete = (
        isinstance(suites, list)
        and [item.get("id") if isinstance(item, dict) else None for item in suites]
        == list(EXPECTED_WP05_PRODUCTION_SUITES)
    )
    if suites_complete:
        for suite, spec in zip(suites, expected_suite_specs, strict=True):
            suite_files = suite.get("dependencies")
            if (
                not isinstance(suite_files, list)
                or not suite_files
                or suite_files != sorted(set(suite_files))
                or not set(suite_files).issubset(EXPECTED_WP05_SOURCE_PATHS)
                or suite.get("translation_unit_count") != len(spec["sources"])
                or suite.get("dependency_count") != len(suite_files)
            ):
                suites_complete = False
                break
            suite_dependency_union.update(suite_files)
    command_plan_complete = False
    if (
        isinstance(expected_cc, str)
        and bool(expected_cc)
        and isinstance(commands, list)
        and len(commands)
        == sum(
            len(spec["sources"])
            for spec in expected_suite_specs
        )
        and all(
            isinstance(command, list)
            and bool(command)
            and all(isinstance(argument, str) for argument in command)
            for command in commands
        )
    ):
        expected_commands = [
            [_canonical_command_argument(argument) for argument in command]
            for command in production_semantic_dependency_command_plan(expected_cc)
        ]
        command_plan_complete = commands == expected_commands
    dependency_complete = (
        dependency.get("verified") is True
        and dependency.get("compiler_generated") is True
        and dependency.get("dependency_mode") == "-MM"
        and dependency.get("dependency_invocation_scope")
        == "one_translation_unit_per_command"
        and dependency.get("source_hash_mode") == "canonical_lf_text_sha256"
        and dependency.get("suite_count") == len(EXPECTED_WP05_PRODUCTION_SUITES)
        and dependency.get("translation_unit_count")
        == sum(
            len(spec["sources"])
            for spec in expected_suite_specs
        )
        and dependency.get("dependency_count") == len(EXPECTED_WP05_SOURCE_PATHS)
        and dependency.get("pinned_source_count") == len(EXPECTED_WP05_SOURCE_PATHS)
        and dependencies == sorted(EXPECTED_WP05_SOURCE_PATHS)
        and suites_complete
        and suite_dependency_union == EXPECTED_WP05_SOURCE_PATHS
        and command_plan_complete
        and dependency.get("unpinned_dependencies") == []
        and dependency.get("unused_pins") == []
        and dependency.get("hash_mismatches") == []
        and dependency.get("failures") == []
    )
    return (
        static_summary_complete
        and value.get("path")
        == "tests/meshcore_oracle/wp05_semantic_matrix.json"
        and value.get("sha256") == sha256_lf_text_file(WP05_SEMANTIC_MATRIX_PATH)
        and value.get("validated") is True
        and value.get("coverage_boundary") == "declared_wp05_host_semantic_matrix"
        and value.get("closure_ready") is False
        and value.get("full_semantic_matrix_covered") is False
        and value.get("requirement_count") == 16
        and value.get("implemented_requirement_count") == 13
        and value.get("partial_requirement_count") == 3
        and value.get("missing_requirement_count") == 0
        and value.get("fuzz_target_count") == 8
        and value.get("implemented_fuzz_target_count") == 8
        and value.get("partial_fuzz_target_count") == 0
        and value.get("missing_fuzz_target_count") == 0
        and value.get("oracle_semantic_vectors") == 915
        and isinstance(value.get("unique_referenced_oracle_semantic_vectors"), int)
        and 0 < value["unique_referenced_oracle_semantic_vectors"] <= 915
        and value.get("production_suite_count")
        == len(EXPECTED_WP05_PRODUCTION_SUITES)
        and value.get("production_scenario_count")
        == sum(
            contract["scenario_count"]
            for contract in EXPECTED_WP05_PRODUCTION_SUITES.values()
        )
        and value.get("companion_upstream_suite_count")
        == len(EXPECTED_WP05_COMPANION_UPSTREAM_SUITES)
        and value.get("companion_upstream_case_count")
        == sum(
            len(contract["case_ids"])
            for contract in EXPECTED_WP05_COMPANION_UPSTREAM_SUITES.values()
        )
        and isinstance(requirements, list)
        and all(isinstance(item, dict) for item in requirements)
        and [item.get("id") for item in requirements]
        == list(EXPECTED_WP05_REQUIREMENT_STATUSES)
        and all(
            item.get("status") == EXPECTED_WP05_REQUIREMENT_STATUSES[item["id"]]
            for item in requirements
        )
        and isinstance(fuzz_targets, list)
        and all(isinstance(item, dict) for item in fuzz_targets)
        and [item.get("id") for item in fuzz_targets]
        == list(EXPECTED_WP05_FUZZ_TARGET_STATUSES)
        and all(
            item.get("status") == EXPECTED_WP05_FUZZ_TARGET_STATUSES[item["id"]]
            for item in fuzz_targets
        )
        and source.get("verified") is True
        and source.get("pins_verified") is True
        and source.get("dependency_closure_verified") is True
        and source.get("source_hash_mode") == "canonical_lf_text_sha256"
        and source.get("failures") == []
        and source_complete
        and dependency_complete
        and wp05_semantic_result_valid(
            value.get("result"), matrix_document["production_host_suites"]
        )
    )


def validate_host_admin_session_receipt(value: Any) -> None:
    if not isinstance(value, dict):
        raise GateFailure("host admin-session receipt is missing")
    required = {
        "schema_version": value.get("schema_version") == 1,
        "artifact_type": value.get("artifact_type")
        == "meshcore_host_admin_session_fixture",
        "receipt_id": value.get("receipt_id")
        == EXPECTED_HOST_ADMIN_SESSION_FIXTURE["receipt_id"],
        "status": value.get("status") == "pass",
        "host_only": value.get("host_only") is True,
        "production_runtime": value.get("production_runtime_proven") is False,
        "wp18": value.get("wp18_closure_eligible") is False,
        "rf": value.get("rf_closure_eligible") is False,
        "ui": value.get("ui_closure_eligible") is False,
        "hardware": value.get("hardware_closure_eligible") is False,
        "upstream": value.get("upstream_commit") == EXPECTED_UPSTREAM["commit"],
        "positive": value.get("positive_checks") == 6,
        "negative": value.get("negative_checks") == 16,
        "source_contract": value.get("source_contract")
        == {
            "login_request": (
                "BaseChatMesh::sendLogin plus Mesh::createAnonDatagram"
            ),
            "login_server": (
                "simple_repeater::handleLoginReq and ClientACL full-key rules"
            ),
            "request": "BaseChatMesh::sendRequest(uint8_t) exact 13-byte schema",
            "response": (
                "simple_repeater::handleRequest reflected LE32 tag plus explicit "
                "little-endian 56-byte RepeaterStats in MyMesh.h order"
            ),
            "direct_and_flood_return": (
                "simple_repeater::onAnonDataRecv and onPeerDataRecv"
            ),
        },
        "transcript": isinstance(value.get("transcript"), list)
        and all(isinstance(entry, dict) for entry in value["transcript"])
        and [entry.get("seq") for entry in value["transcript"]]
        == list(range(1, 12)),
        "invariants": value.get("invariants")
        == {
            "identity_bytes": 32,
            "prefix_authorization": False,
            "login_payload_type": 7,
            "request_payload_type": 0,
            "response_payload_type": 1,
            "admin_permissions": 3,
            "login_response_bytes": 13,
            "get_status_request_bytes": 13,
            "repeater_stats_bytes": 56,
            "repeater_stats_fields": 18,
            "get_status_response_bytes": 60,
            "get_status_type": 1,
            "login_timestamp": 0x01020304,
            "request_tag": 0x01020305,
        },
        "canonical_bytes": value.get("canonical_bytes")
        == {
            "get_status_request_hex": "050302010100000000a1b2c3d4",
            "login_response_hex": "88776655000001031020304002",
            "get_status_response_hex": HOST_ADMIN_GET_STATUS_RESPONSE_HEX,
        },
        "repeater_stats": value.get("repeater_stats")
        == EXPECTED_HOST_ADMIN_REPEATER_STATS,
        "zeroization": value.get("zeroization")
        == {
            "timeout_pending_state": True,
            "timeout_pending_login_state": True,
            "explicit_logout_client_server_session": True,
        },
    }
    negative = value.get("negative_matrix")
    required["negative_matrix"] = (
        isinstance(negative, list)
        and all(isinstance(entry, dict) for entry in negative)
        and [entry.get("id") for entry in negative]
        == HOST_ADMIN_NEGATIVE_MATRIX
        and all(entry.get("rejected") is True for entry in negative)
    )
    failed = [name for name, passed in required.items() if not passed]
    if failed:
        raise GateFailure(
            "host admin-session receipt drifted: " + ", ".join(failed)
        )


def completed_fuzz_runs(stderr: str, requested: int) -> int | None:
    final_stats = re.findall(r"stat::number_of_executed_units:\s*(\d+)", stderr)
    if final_stats:
        return int(final_stats[-1])
    done_counts = re.findall(r"#(\d+)\s+DONE", stderr)
    if done_counts:
        return int(done_counts[-1])
    if requested == 0 and "Done 0 runs" in stderr:
        return 0
    return None


def base_report(
    manifest: dict[str, Any],
    oracle_manifest: dict[str, Any],
    oracle_coverage: dict[str, Any],
    wp05_semantic_matrix: dict[str, Any],
    args: argparse.Namespace,
) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "artifact_type": "d1l_meshcore_wire_conformance",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "passed": False,
        "status": "not_run",
        "execution_complete": False,
        "coverage_boundary": "wire_envelope_only",
        "coverage_level": "wire_envelope_only",
        "semantic_coverage_level": "partial_declared_host_semantics",
        "issue_65_closure_eligible": False,
        "closure_ready": False,
        "excluded_coverage": {
            "crypto_vectors": 0,
            "retained_state_vectors": 0,
            "hardware_vectors": 0,
            "rf_vectors": 0,
            "packet_semantics_vectors": 0,
        },
        "scope": {
            "vector_oracle": "pinned_Packet_wire_read_write_only",
            "upstream_oracle_interface": ORACLE_COVERAGE_BOUNDARY,
            "fuzz_target": "local_wire_decoder_only",
            "semantic_fuzz_targets": ["local_advert_parser"],
            "advert_parser_fuzz_covered": True,
            "packet_semantics_covered": False,
            "bounded_production_semantics_covered": True,
            "crypto_oracle_available": False,
            "public_group_crypto_oracle_available": True,
            "dm_crypto_oracle_available": True,
            "expected_ack_path_oracle_available": True,
            "path_return_route_codes_oracle_available": True,
            "production_host_semantic_suites": list(
                EXPECTED_WP05_PRODUCTION_SUITES
            ),
            "companion_upstream_semantic_suites": list(
                EXPECTED_WP05_COMPANION_UPSTREAM_SUITES
            ),
        },
        "requested": {
            "commit": args.commit,
            "seed": args.seed,
            "fuzz_runs": args.runs,
            "advert_fuzz_runs": args.runs,
            "sanitizers": ["address", "undefined"],
        },
        "sanitizer_policy": oracle_manifest["sanitizer_policy"],
        "full_ubsan_clean": True,
        "sanitizer_policy_passed": None,
        "manifest": {
            "path": str(MANIFEST_PATH.relative_to(ROOT)).replace("\\", "/"),
            "sha256": sha256_file(MANIFEST_PATH),
            "upstream_commit": manifest["upstream"]["commit"],
        },
        "oracle": {
            "coverage_boundary": ORACLE_COVERAGE_BOUNDARY,
            "static_manifest": {
                "path": str(ORACLE_MANIFEST_PATH.relative_to(ROOT)).replace(
                    "\\", "/"
                ),
                "sha256": sha256_file(ORACLE_MANIFEST_PATH),
                "corpus_version": ORACLE_CORPUS_VERSION,
                "abi_version": ORACLE_ABI_VERSION,
                "upstream_commit": oracle_manifest["upstream"]["commit"],
            },
            "capabilities": oracle_manifest["capabilities"],
            "coverage_policy": summarize_oracle_coverage(
                oracle_coverage, oracle_manifest
            ),
            "source_verification": None,
            "result": None,
            "artifact_path": None,
        },
        "wp05_semantic_matrix": summarize_wp05_semantic_matrix(
            wp05_semantic_matrix, oracle_manifest
        ),
        "signed_advert_runtime": None,
        "vector_matrix": manifest["vector_matrix"],
        "payload_version_gate": manifest["payload_version_gate"],
        "corpus": None,
        "advert_corpus": None,
        "commands": command_plan(args.cc, args.cxx),
        "source_verification": None,
        "vector_result": None,
        "fuzz_command": None,
        "fuzz_result": None,
        "advert_fuzz_command": None,
        "advert_fuzz_result": None,
        "toolchain": None,
        "sanitizer_errors": None,
        "memory_errors": None,
        "zero_overrun": None,
        "zero_corruption": None,
        "failure": None,
    }


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def build_oracle_artifact(report: dict[str, Any]) -> dict[str, Any]:
    oracle = report["oracle"]
    result = oracle["result"]
    source_verification = oracle["source_verification"]
    execution_complete = bool(report.get("execution_complete") and result)
    passed = bool(
        report.get("passed")
        and execution_complete
        and result.get("passed") is True
        and source_verification
        and source_verification.get("verified") is True
    )
    return {
        "schema_version": 1,
        "artifact_type": "meshcore_oracle_manifest",
        "generated_at": report["generated_at"],
        "status": "pass" if passed else report.get("status", "fail"),
        "passed": passed,
        "execution_complete": execution_complete,
        "coverage_boundary": ORACLE_COVERAGE_BOUNDARY,
        "corpus_version": ORACLE_CORPUS_VERSION,
        "abi_version": ORACLE_ABI_VERSION,
        "wp04_closure_eligible": False,
        "closure_ready": False,
        "wp04_acceptance_ready": False,
        "sanitizer_policy": report["sanitizer_policy"],
        "full_ubsan_clean": report["full_ubsan_clean"],
        "sanitizer_policy_passed": report.get("sanitizer_policy_passed"),
        "coverage_policy": oracle["coverage_policy"],
        "repository_commit": (report.get("source_verification") or {}).get(
            "repository_commit"
        ),
        "upstream_commit": oracle["static_manifest"]["upstream_commit"],
        "static_manifest": oracle["static_manifest"],
        "source_verification": source_verification,
        "oracle_result": result,
        "capabilities": oracle["capabilities"],
        "wp05_semantic_matrix": report["wp05_semantic_matrix"],
        "signed_advert_runtime": report.get("signed_advert_runtime"),
        "pending_capabilities": [
            capability["id"]
            for capability in oracle["capabilities"]
            if capability["status"] == "pending"
        ],
        "failure": report.get("failure"),
    }


def execute(args: argparse.Namespace) -> tuple[int, dict[str, Any]]:
    manifest = load_manifest()
    oracle_manifest = load_oracle_manifest()
    oracle_coverage = load_oracle_coverage_manifest(oracle_manifest, manifest)
    wp05_semantic_matrix = load_wp05_semantic_matrix(oracle_manifest)
    report = base_report(
        manifest,
        oracle_manifest,
        oracle_coverage,
        wp05_semantic_matrix,
        args,
    )
    signed_advert_receipt: object | None = None
    try:
        if args.dry_run and os.environ.get("D1L_MESHCORE_CONFORMANCE_CI") == "1":
            raise GateFailure("dry-run is forbidden in GitHub Actions")
        if args.runs != DEFAULT_RUNS:
            raise GateFailure(f"release gate requires exactly {DEFAULT_RUNS} fuzz runs")
        if args.seed != DEFAULT_SEED:
            raise GateFailure(f"release gate requires deterministic seed {DEFAULT_SEED}")
        source_verification = verify_sources(manifest, args.commit)
        report["source_verification"] = source_verification
        if args.signed_advert_runtime_receipt is not None:
            try:
                signed_advert_receipt = json.loads(
                    args.signed_advert_runtime_receipt.read_text(encoding="utf-8")
                )
                report["signed_advert_runtime"] = bind_signed_advert_runtime_receipt(
                    signed_advert_receipt,
                    source_verification["repository_commit"],
                )
            except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
                raise GateFailure(
                    f"signed-advert runtime receipt validation failed: {exc}"
                ) from exc
        report["oracle"]["source_verification"] = verify_oracle_sources(
            oracle_manifest
        )
        report["wp05_semantic_matrix"]["source_verification"] = (
            verify_wp05_semantic_sources(wp05_semantic_matrix)
        )
        _corpus, corpus_seeds = load_corpus(manifest)
        _advert_corpus, advert_corpus_seeds = load_advert_corpus(manifest)
        report["corpus"] = {
            "path": str(CORPUS_PATH.relative_to(ROOT)).replace("\\", "/"),
            "manifest_sha256": sha256_lf_text_file(CORPUS_PATH),
            "coverage_boundary": "local_wire_decoder_only",
            "seed_count": len(corpus_seeds),
            "seeds": [
                {"name": name, "size": len(payload), "sha256": digest}
                for name, payload, digest in corpus_seeds
            ],
            "verified": True,
        }
        report["advert_corpus"] = {
            "path": str(ADVERT_CORPUS_PATH.relative_to(ROOT)).replace("\\", "/"),
            "manifest_sha256": sha256_lf_text_file(ADVERT_CORPUS_PATH),
            "coverage_boundary": "local_advert_parser",
            "seed_count": len(advert_corpus_seeds),
            "seeds": [
                {"name": name, "size": len(payload), "sha256": digest}
                for name, payload, digest in advert_corpus_seeds
            ],
            "verified": True,
        }

        if args.dry_run:
            report["status"] = "dry_run"
            report["failure"] = "dry-run does not satisfy the conformance gate"
            return 0, report
        if args.signed_advert_runtime_receipt is None:
            raise GateFailure(
                "real conformance runs require an exact-commit signed-advert runtime receipt"
            )
        if os.name == "nt":
            raise GateFailure("real sanitizer conformance runs require the Ubuntu CI job")

        if args.toolchain_receipt is None:
            raise GateFailure("real conformance runs require a Clang tool-byte receipt")
        try:
            toolchain = json.loads(args.toolchain_receipt.read_text(encoding="utf-8"))
            metadata = load_build_inputs(BUILD_INPUTS_PATH)
            validate_clang_receipt(
                toolchain,
                metadata,
                source_verification["repository_commit"],
                sha256_file(BUILD_INPUTS_PATH),
            )
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
            raise GateFailure(f"Clang tool-byte receipt validation failed: {exc}") from exc
        report["toolchain"] = toolchain

        cc_identity = compiler_identity(args.cc)
        cxx_identity = compiler_identity(args.cxx)
        sanitizer_env = os.environ.copy()
        sanitizer_env["ASAN_OPTIONS"] = (
            "abort_on_error=1:detect_leaks=1:halt_on_error=1:strict_string_checks=1"
        )
        sanitizer_env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        with tempfile.TemporaryDirectory(prefix="d1l-meshcore-conformance-") as temporary:
            build_dir = Path(temporary)
            corpus_dir = build_dir / "corpus"
            corpus_dir.mkdir()
            for index, (name, payload, _digest) in enumerate(corpus_seeds):
                (corpus_dir / f"{index:02d}_{name}").write_bytes(payload)
            advert_corpus_dir = build_dir / "advert-corpus"
            advert_corpus_dir.mkdir()
            for index, (name, payload, _digest) in enumerate(advert_corpus_seeds):
                (advert_corpus_dir / f"{index:02d}_{name}").write_bytes(payload)
            commands = command_plan(args.cc, args.cxx, str(build_dir))
            report["commands"] = commands
            for command in commands:
                run_process(command, timeout=args.timeout_sec, env=sanitizer_env)

            dependency_outputs = [
                run_process(
                    command,
                    timeout=args.timeout_sec,
                    env=sanitizer_env,
                ).stdout
                for command in production_semantic_dependency_command_plan(args.cc)
            ]
            dependency_verification = verify_wp05_semantic_dependencies(
                wp05_semantic_matrix,
                dependency_outputs,
                args.cc,
            )
            finalize_wp05_semantic_source_verification(
                report["wp05_semantic_matrix"]["source_verification"],
                dependency_verification,
            )

            oracle = run_process(
                [str(build_dir / "meshcore_oracle_vectors")],
                timeout=args.timeout_sec,
                env=sanitizer_env,
            )
            oracle_result = parse_oracle_output(oracle.stdout)
            report["oracle"]["result"] = oracle_result
            validate_host_admin_session_receipt(
                oracle_result.get("admin_session_fixture")
            )
            if (
                oracle_result.get("passed") is not True
                or oracle_result.get("coverage_boundary")
                != ORACLE_COVERAGE_BOUNDARY
                or oracle_result.get("wp04_closure_eligible") is not False
                or oracle_result.get("abi_version") != ORACLE_ABI_VERSION
                or oracle_result.get("upstream_commit")
                != EXPECTED_UPSTREAM["commit"]
                or oracle_result.get("vectors") != oracle_manifest["vectors"]
                or oracle_result.get("capabilities")
                != {
                    "packet_envelope": True,
                    "advert_data_fields": True,
                    "signed_advert_packet_creation": True,
                    "signed_advert_verification": True,
                    "ed25519_point_validation": True,
                    "identity_shared_secret_derivation": True,
                    "public_group_packets": True,
                    "anonymous_login_request_packets": True,
                    "regular_request_response_packets": True,
                    "canonical_login_response_packets": True,
                    "login_password_authorization_fixtures": True,
                    "existing_acl_blank_login_reuse_fixtures": True,
                    "authorized_login_acl_transition_fixtures": True,
                    "authenticated_request_replay_transition_fixtures": True,
                    "authenticated_text_replay_response_session_fixtures": True,
                    "login_response_creation_dispatch_orchestration_fixtures": True,
                    "signed_advert_dispatch_transition_fixtures": True,
                    "signed_advert_creation_send_orchestration_fixtures": True,
                    "dm_encrypt_decrypt": True,
                    "expected_ack_hash_and_ack_path": True,
                    "path_return_route_codes": True,
                    "direct_flood_headers": True,
                    "ack_frames": True,
                    "trace_source_frames": True,
                }
                or oracle_result.get("failures") != 0
            ):
                raise GateFailure("MeshCore oracle result drifted from its fixed matrix")

            harness = run_process(
                [str(build_dir / "meshcore_wire_conformance")],
                timeout=args.timeout_sec,
                env=sanitizer_env,
            )
            vector_result = parse_harness_output(harness.stdout)
            report["vector_result"] = vector_result
            if (
                vector_result.get("passed") is not True
                or vector_result.get("coverage_boundary") != "wire_envelope_only"
                or vector_result.get("issue_65_closure_eligible") is not False
                or vector_result.get("vectors", {}).get("upstream_to_local") != 504
                or vector_result.get("vectors", {}).get("local_to_upstream") != 504
                or vector_result.get("vectors", {}).get("total") != 1008
                or vector_result.get("path_length_encodings", {}).get("tested") != 256
                or vector_result.get("path_length_encodings", {}).get("valid") != 119
                or vector_result.get("path_length_encodings", {}).get("invalid") != 137
                or vector_result.get("payload_version_gate", {}).get("tested") != 4
                or vector_result.get("payload_version_gate", {}).get("accepted_v1") != 1
                or vector_result.get("payload_version_gate", {}).get("rejected_future") != 3
                or vector_result.get("payload_version_gate", {}).get("preserved_rejections") != 3
                or vector_result.get("undersized_encoder_capacities") != 254
                or vector_result.get("failures") != 0
            ):
                raise GateFailure("vector harness result did not satisfy the fixed matrix")

            semantic_result = run_wp05_semantic_suites(
                wp05_semantic_matrix,
                build_dir,
                timeout=args.timeout_sec,
                env=sanitizer_env,
            )
            if not wp05_semantic_result_valid(
                semantic_result,
                wp05_semantic_matrix["production_host_suites"],
            ):
                raise GateFailure("WP-05 production semantic matrix result drifted")
            report["wp05_semantic_matrix"]["result"] = semantic_result

            findings_dir = (
                args.out.parent
                / f"meshcore_conformance_findings_{source_verification['repository_commit']}"
            ).resolve()
            findings_dir.mkdir(parents=True, exist_ok=True)
            if any(findings_dir.iterdir()):
                raise GateFailure(f"fuzzer findings directory is not empty: {findings_dir}")
            fuzz_command = [
                str(build_dir / "meshcore_wire_fuzz"),
                f"-runs={args.runs}",
                f"-seed={args.seed}",
                "-max_len=255",
                "-print_final_stats=1",
                f"-artifact_prefix={findings_dir}{os.sep}",
                str(corpus_dir),
            ]
            fuzz_started = time.monotonic()
            try:
                fuzz = run_process(
                    fuzz_command,
                    timeout=args.timeout_sec,
                    env=sanitizer_env,
                )
            except GateFailure:
                report["fuzz_result"] = {
                    "passed": False,
                    "engine": "libFuzzer",
                    "seed": args.seed,
                    "requested_runs": args.runs,
                    "completed_runs": None,
                    "duration_ms": round((time.monotonic() - fuzz_started) * 1000),
                    "max_input_bytes": 255,
                    "sanitizers": ["address", "undefined"],
                    "artifact_prefix": str(findings_dir),
                    "finding_files": sorted(path.name for path in findings_dir.iterdir()),
                }
                raise
            fuzz_duration_ms = round((time.monotonic() - fuzz_started) * 1000)
            completed = completed_fuzz_runs(fuzz.stderr, args.runs)
            finding_files = sorted(path.name for path in findings_dir.iterdir())
            report["fuzz_command"] = fuzz_command
            report["fuzz_result"] = {
                "passed": completed == args.runs,
                "engine": "libFuzzer",
                "seed": args.seed,
                "requested_runs": args.runs,
                "completed_runs": completed,
                "duration_ms": fuzz_duration_ms,
                "max_input_bytes": 255,
                "sanitizers": ["address", "undefined"],
                "artifact_prefix": str(findings_dir),
                "finding_files": finding_files,
                "findings": 0 if completed == args.runs and not finding_files else None,
            }
            if completed != args.runs:
                raise GateFailure(
                    f"libFuzzer completion count was {completed}, expected {args.runs}"
                )
            if finding_files:
                raise GateFailure("libFuzzer emitted finding artifacts despite a zero exit")

            advert_findings_dir = (
                args.out.parent
                / (
                    "meshcore_advert_fuzz_findings_"
                    f"{source_verification['repository_commit']}"
                )
            ).resolve()
            advert_findings_dir.mkdir(parents=True, exist_ok=True)
            if any(advert_findings_dir.iterdir()):
                raise GateFailure(
                    "advert fuzzer findings directory is not empty: "
                    f"{advert_findings_dir}"
                )
            advert_fuzz_command = [
                str(build_dir / "meshcore_advert_fuzz"),
                f"-runs={args.runs}",
                f"-seed={args.seed}",
                "-max_len=33",
                "-print_final_stats=1",
                f"-artifact_prefix={advert_findings_dir}{os.sep}",
                str(advert_corpus_dir),
            ]
            advert_fuzz_started = time.monotonic()
            try:
                advert_fuzz = run_process(
                    advert_fuzz_command,
                    timeout=args.timeout_sec,
                    env=sanitizer_env,
                )
            except GateFailure:
                report["advert_fuzz_command"] = advert_fuzz_command
                report["advert_fuzz_result"] = {
                    "passed": False,
                    "engine": "libFuzzer",
                    "target": "local_advert_parser",
                    "invalid_output_policy": "canonical_zeroed_with_type_N",
                    "seed": args.seed,
                    "requested_runs": args.runs,
                    "completed_runs": None,
                    "duration_ms": round(
                        (time.monotonic() - advert_fuzz_started) * 1000
                    ),
                    "max_input_bytes": 33,
                    "sanitizers": ["address", "undefined"],
                    "artifact_prefix": str(advert_findings_dir),
                    "finding_files": sorted(
                        path.name for path in advert_findings_dir.iterdir()
                    ),
                }
                raise
            advert_fuzz_duration_ms = round(
                (time.monotonic() - advert_fuzz_started) * 1000
            )
            advert_completed = completed_fuzz_runs(advert_fuzz.stderr, args.runs)
            advert_finding_files = sorted(
                path.name for path in advert_findings_dir.iterdir()
            )
            report["advert_fuzz_command"] = advert_fuzz_command
            report["advert_fuzz_result"] = {
                "passed": advert_completed == args.runs,
                "engine": "libFuzzer",
                "target": "local_advert_parser",
                "invalid_output_policy": "canonical_zeroed_with_type_N",
                "seed": args.seed,
                "requested_runs": args.runs,
                "completed_runs": advert_completed,
                "duration_ms": advert_fuzz_duration_ms,
                "max_input_bytes": 33,
                "sanitizers": ["address", "undefined"],
                "artifact_prefix": str(advert_findings_dir),
                "finding_files": advert_finding_files,
                "findings": (
                    0
                    if advert_completed == args.runs and not advert_finding_files
                    else None
                ),
            }
            if advert_completed != args.runs:
                raise GateFailure(
                    "advert libFuzzer completion count was "
                    f"{advert_completed}, expected {args.runs}"
                )
            if advert_finding_files:
                raise GateFailure(
                    "advert libFuzzer emitted finding artifacts despite a zero exit"
                )

        report["compiler"] = {"cc": cc_identity, "cxx": cxx_identity}
        report["sanitizer_errors"] = 0
        report["sanitizer_policy_passed"] = True
        report["memory_errors"] = 0
        report["zero_overrun"] = True
        report["zero_corruption"] = True
        report["execution_complete"] = True
        report["passed"] = True
        report["status"] = "pass"
        try:
            validate_completed_report(
                report,
                source_verification["repository_commit"],
                signed_advert_runtime_receipt=signed_advert_receipt,
            )
        except ValueError as exc:
            raise GateFailure(str(exc)) from exc
        return 0, report
    except GateFailure as exc:
        report["status"] = "fail"
        report["passed"] = False
        report["execution_complete"] = False
        report["failure"] = str(exc)
        return 1, report


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang", help="Clang C compiler")
    parser.add_argument("--cxx", default="clang++", help="Clang C++ compiler")
    parser.add_argument("--commit", help="exact repository commit expected by the gate")
    parser.add_argument("--toolchain-receipt", type=Path)
    parser.add_argument("--signed-advert-runtime-receipt", type=Path)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--runs", "--fuzz-runs", dest="runs", type=int, default=DEFAULT_RUNS)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--oracle-out",
        type=Path,
        help="output path for the exact-commit MeshCore oracle artifact",
    )
    parser.add_argument("--timeout-sec", type=int, default=300)
    parser.add_argument("--require-sanitizers", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if args.timeout_sec < 1:
        parser.error("--timeout-sec must be positive")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        status, report = execute(args)
    except (GateFailure, OSError, json.JSONDecodeError) as exc:
        report = {
            "schema_version": 1,
            "artifact_type": "d1l_meshcore_wire_conformance",
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "passed": False,
            "status": "fail",
            "execution_complete": False,
            "coverage_boundary": "wire_envelope_only",
            "coverage_level": "wire_envelope_only",
            "semantic_coverage_level": "partial_declared_host_semantics",
            "issue_65_closure_eligible": False,
            "closure_ready": False,
            "failure": str(exc),
        }
        status = 1
    oracle_artifact = None
    if "oracle" in report:
        repository_commit = (report.get("source_verification") or {}).get(
            "repository_commit"
        )
        artifact_commit = repository_commit or args.commit or "unknown"
        oracle_out = args.oracle_out or (
            args.out.parent / f"meshcore_oracle_manifest_{artifact_commit}.json"
        )
        report["oracle"]["artifact_path"] = str(oracle_out)
        oracle_artifact = build_oracle_artifact(report)
    write_report(args.out, report)
    if oracle_artifact is not None:
        write_report(oracle_out, oracle_artifact)
    print(json.dumps(report, sort_keys=True))
    return status


if __name__ == "__main__":
    sys.exit(main())
