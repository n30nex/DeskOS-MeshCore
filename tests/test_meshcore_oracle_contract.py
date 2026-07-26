import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from scripts import meshcore_conformance_d1l as conformance


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "meshcore_conformance_d1l.py"
ORACLE_ROOT = ROOT / "tests" / "meshcore_oracle"
MANIFEST = ORACLE_ROOT / "manifest.json"
COVERAGE_MANIFEST = ORACLE_ROOT / "coverage_manifest.json"
UPSTREAM_COMMIT = "e8d3c53ba1ea863937081cd0caad759b832f3028"
BOUNDARY = (
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


def canonical_lf_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def git_head() -> str:
    return subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def test_oracle_manifest_is_exactly_pinned_and_fail_closed():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert manifest["schema_version"] == 1
    assert manifest["corpus_version"] == 24
    assert manifest["abi_version"] == 2
    assert manifest["coverage_boundary"] == BOUNDARY
    assert manifest["wp04_closure_eligible"] is False
    assert manifest["sanitizer_policy"] == conformance.ED25519_SANITIZER_POLICY
    assert manifest["upstream"]["commit"] == UPSTREAM_COMMIT
    repeater_stats_header = (
        "third_party/MeshCore/examples/simple_repeater/MyMesh.h"
    )
    repeater_stats_header_sha256 = (
        "a48c6c0827687d18472c3652401a2a2a9d544f2b884f1518e52812dfd78a22f6"
    )
    assert manifest["upstream"]["sources"][repeater_stats_header] == (
        repeater_stats_header_sha256
    )
    assert repeater_stats_header in conformance.EXPECTED_ORACLE_UPSTREAM_SOURCE_PATHS
    assert repeater_stats_header in manifest["host_admin_session_fixture"][
        "pinned_upstream_sources"
    ]
    assert canonical_lf_sha256(ROOT / repeater_stats_header) == (
        repeater_stats_header_sha256
    )
    assert (
        manifest["host_admin_session_fixture"]
        == conformance.EXPECTED_HOST_ADMIN_SESSION_FIXTURE
    )
    assert manifest["interface"] == {
        "language": "c_abi",
        "header": "tests/meshcore_oracle/meshcore_oracle.h",
        "implementation": "tests/meshcore_oracle/meshcore_oracle.cpp",
        "upstream_types": [
            "mesh::Packet",
            "mesh::Utils",
            "AdvertDataBuilder",
            "AdvertDataParser",
        ],
        "reject_preserves_output": True,
        "crypto_available": False,
        "group_crypto_available": True,
        "group_crypto_scope": (
            "aes128_ecb_zero_padding_truncated_hmac_sha256_and_"
            "basechatmesh_setchannel_hash_only"
        ),
        "dm_crypto_available": True,
        "dm_crypto_scope": (
            "basechatmesh_plain_text_layout_and_mesh_datagram_crypto_with_"
            "caller_supplied_hashes_and_shared_secret_only"
        ),
        "expected_ack_path_available": True,
        "expected_ack_path_scope": (
            "plain_dm_expected_hash_all_lengths_defined_ack_bodies_and_"
            "authenticated_ack_path_with_caller_supplied_identity_hash_"
            "secret_and_rng_inputs_only"
        ),
        "path_return_route_codes_available": True,
        "path_return_route_codes_scope": (
            "general_authenticated_path_return_extra_and_no_extra_uniqueness_"
            "framing_with_caller_supplied_hash_secret_rng_and_route_inputs_"
            "only_no_route_selection_dispatch_forwarding_or_rf"
        ),
        "signed_advert_ed25519_available": True,
        "signed_advert_scope": (
            "d1l_production_message_layout_strict_points_and_ed25519_verification_only_no_mesh_dispatch"
        ),
        "signed_advert_packet_creation_available": True,
        "signed_advert_packet_creation_scope": (
            "deterministic_seed_keypair_pre_route_creation_and_authenticated_"
            "parse_only_no_identity_verifier_dispatch_replay_contact_or_rf"
        ),
        "anonymous_login_request_available": True,
        "anonymous_login_request_scope": (
            "canonical_room_nonroom_login_request_create_authenticated_parse_"
            "with_caller_supplied_hash_public_key_secret_time_and_room_mode_"
            "only_no_key_exchange_authorization_replay_response_session_"
            "dispatch_route_or_rf"
        ),
        "regular_request_response_available": True,
        "regular_request_response_scope": (
            "authenticated_nonempty_req_response_create_parse_with_caller_"
            "supplied_type_hashes_secret_and_logical_length_only_no_schema_"
            "tag_authorization_replay_session_dispatch_route_or_rf"
        ),
        "identity_shared_secret_derivation_available": True,
        "identity_shared_secret_derivation_scope": (
            "valid_input_seed_keypair_strict_peer_edwards_to_montgomery_shared_"
            "secret_with_zero_rejection_only_no_persisted_key_contact_auth_"
            "session_dispatch_route_production_integration_or_rf"
        ),
        "canonical_login_response_available": True,
        "canonical_login_response_scope": (
            "repeater_room_success_response_schema_authenticated_packet_with_"
            "caller_supplied_hashes_secret_time_permissions_and_uniqueness_"
            "only_no_password_acl_replay_session_transition_dispatch_route_or_rf"
        ),
        "login_password_authorization_available": True,
        "login_password_authorization_scope": (
            "repeater_room_acl_miss_password_admin_guest_read_only_"
            "authorization_and_denial_only_no_existing_acl_contact_mutation_"
            "replay_secret_session_response_dispatch_route_or_rf"
        ),
        "existing_acl_blank_login_reuse_available": True,
        "existing_acl_blank_login_reuse_scope": (
            "repeater_room_blank_password_full_public_key_first_match_acl_"
            "reuse_response_secret_selection_and_client_out_path_mutation_"
            "boundary_only_no_storage_load_save_insert_evict_permissions_"
            "secret_timestamp_activity_replay_session_response_creation_"
            "dispatch_route_or_rf"
        ),
        "authorized_login_acl_transition_available": True,
        "authorized_login_acl_transition_scope": (
            "clientacl_putclient_full_key_reuse_append_least_active_non_admin_"
            "evict_and_repeater_room_authorized_login_record_transition_"
            "projection_only_no_filesystem_full_path_bytes_unmodified_room_"
            "scheduling_identity_signature_secret_derivation_password_"
            "response_dispatch_route_or_rf"
        ),
        "authenticated_request_replay_transition_available": True,
        "authenticated_request_replay_transition_scope": (
            "repeater_room_prevalidated_authenticated_request_timestamp_"
            "duplicate_handler_result_keep_alive_and_record_session_"
            "transition_projection_only_no_request_schema_handler_execution_"
            "response_hash_creation_storage_identity_secret_dispatch_route_"
            "schedule_or_rf"
        ),
        "authenticated_text_transition_available": True,
        "authenticated_text_transition_scope": (
            "repeater_room_canonical_authenticated_text_newer_equal_older_"
            "role_handler_post_session_ack_response_creation_outcome_and_"
            "stored_path_dispatch_projection_only_no_decryption_text_parse_"
            "hash_handler_storage_packet_pool_dispatch_timing_route_execution_"
            "identity_secret_or_rf"
        ),
        "login_response_dispatch_transition_available": True,
        "login_response_dispatch_transition_scope": (
            "repeater_room_canonical_response_ready_encoded_path_creation_"
            "kind_secret_source_exact_2000ms_room_push_and_300ms_coarse_"
            "dispatch_projection_only_flood_transport_scope_required_no_"
            "packet_pool_path_copy_clock_queue_dispatch_execution_storage_or_rf"
        ),
        "signed_advert_dispatch_transition_available": True,
        "signed_advert_dispatch_transition_scope": (
            "mesh_post_decode_and_flood_filter_pass_complete_self_seen_"
            "caller_supplied_identity_"
            "result_callback_and_canonical_flood_route_capacity_projection_"
            "with_direct_nonzero_intercept_and_d1l_overlong_and_path_count_"
            "wrap_reject_only_"
            "upstream_identity_parity_false_no_external_verifier_tables_path_"
            "copy_clock_queue_dispatch_or_rf"
        ),
        "signed_advert_send_transition_available": True,
        "signed_advert_send_transition_scope": (
            "mesh_createadvert_length_pool_error_rtc_sign_then_flood_transport_"
            "direct_zero_hop_ownership_queue_order_projection_only_signature_"
            "bytes_covered_separately_no_keypair_consistency_self_verify_"
            "packet_pool_clock_table_path_copy_queue_dispatch_or_rf"
        ),
        "canonical_advert_data": True,
        "route_header_scope": "non_trace_direct_flood_and_zero_hop_headers",
        "ack_frame_scope": "simple_and_multipart_payload_framing_only",
        "trace_frame_scope": (
            "initial_outbound_direct_flags_zero_trace_framing_only"
        ),
        "golden_vector_sources": {
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
                "two exact payloads and ten-vector matrix SHA-256 from independent "
                "AES-128 ECB and HMAC-SHA-256 generation"
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
            "signed_advert_dispatch_mesh": "third_party/MeshCore/src/Mesh.cpp",
            "signed_advert_dispatch_identity_boundary": (
                "third_party/MeshCore/src/Identity.cpp"
            ),
            "signed_advert_dispatch_route": "third_party/MeshCore/src/Mesh.cpp",
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
            "public_group_encrypt_mac_parse": (
                "third_party/MeshCore/src/Utils.cpp"
            ),
            "public_group_aes": (
                "third_party/sensecap_indicator_esp32/components/LoRaWAN/"
                "soft-se/aes.c"
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
        },
    }
    assert manifest["determinism"]["signed_advert_seed_hex"] == (
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f"
    )
    assert manifest["determinism"]["signed_advert_generation_recipe"] == (
        "ed25519_create_keypair_seed_then_ed25519_sign_"
        "public_key_timestamp_wire_bytes_app_data"
    )
    assert manifest["determinism"]["anonymous_login_recipe"] == (
        "basechatmesh_timestamp_optional_room_sync_password_then_mesh_"
        "anonymous_datagram_aes128_hmac_with_caller_supplied_outer_and_secret"
    )
    assert manifest["determinism"]["anonymous_login_matrix"] == (
        "nonroom_empty_exact12_max15_and_room_empty_exact8_max15"
    )
    assert manifest["determinism"]["regular_request_response_recipe"] == (
        "mesh_req_response_hash_prefix_then_utils_aes128_ecb_zero_pad_and_two_"
        "byte_hmac_sha256_with_caller_logical_length"
    )
    assert manifest["determinism"]["regular_request_response_matrix"] == (
        "request_lengths_1_15_16_17_and_response_lengths_16_167"
    )
    assert manifest["determinism"]["identity_exchange_recipe"] == (
        "ed25519_seed_keypair_strict_peer_y_to_montgomery_ladder_zero_secret_"
        "reject_and_temp_wipe"
    )
    assert manifest["determinism"]["identity_exchange_matrix"] == (
        "five_symmetric_seed_pairs_including_all_zero_seed_independently_"
        "generated_with_libsodium"
    )
    assert manifest["determinism"]["canonical_login_response_recipe"] == (
        "repeater_or_room_success_schema_then_mesh_response_datagram_aes128_"
        "hmac_with_caller_supplied_hashes_secret_time_permissions_and_uniqueness"
    )
    assert manifest["determinism"]["canonical_login_response_matrix"] == (
        "repeater_and_room_guest_read_only_read_write_admin_and_flagged_role_"
        "permissions"
    )
    assert manifest["determinism"]["login_password_authorization_recipe"] == (
        "repeater_room_acl_miss_admin_first_guest_second_optional_room_read_"
        "only_fallback"
    )
    assert manifest["determinism"]["login_password_authorization_matrix"] == (
        "sixteen_allow_deny_precedence_empty_maximum_and_role_cases"
    )
    assert manifest["determinism"]["existing_acl_blank_login_recipe"] == (
        "blank_password_full_public_key_first_match_then_server_secret_source_"
        "and_flood_out_path_mutation_classification"
    )
    assert manifest["determinism"]["existing_acl_blank_login_matrix"] == (
        "twelve_empty_first_middle_last_duplicate_prefix_full_capacity_match_"
        "miss_route_and_server_cases"
    )
    assert manifest["determinism"]["authorized_login_acl_transition_recipe"] == (
        "clientacl_putclient_full_key_reuse_append_or_strict_least_active_non_"
        "admin_evict_then_replay_gate_and_server_record_field_updates"
    )
    assert manifest["determinism"]["authorized_login_acl_transition_matrix"] == (
        "sixteen_append_existing_replay_capacity_eviction_tie_all_admin_role_"
        "secret_time_room_state_dirty_and_flood_cases"
    )
    assert manifest["determinism"][
        "authenticated_request_replay_transition_recipe"
    ] == (
        "prevalidated_request_repeater_strict_newer_and_handler_commit_vs_"
        "room_equal_accept_prehandler_commit_and_direct_keep_alive_session_"
        "update"
    )
    assert manifest["determinism"][
        "authenticated_request_replay_transition_matrix"
    ] == (
        "fifteen_repeater_room_newer_equal_older_handler_success_failure_keep_"
        "alive_force_since_path_and_timestamp_boundary_cases"
    )
    assert manifest["determinism"]["authenticated_text_transition_recipe"] == (
        "canonical_text_repeater_admin_gate_and_room_role_gate_then_equal_"
        "retry_session_commit_handler_or_post_suppression_creation_outcome_"
        "and_stored_path_dispatch_projection"
    )
    assert manifest["determinism"]["authenticated_text_transition_matrix"] == (
        "twenty_repeater_room_plain_cli_newer_equal_older_role_empty_max_"
        "timestamp_creation_failure_ack_multiplicity_and_path_cases"
    )
    assert manifest["determinism"]["login_response_dispatch_recipe"] == (
        "ready_canonical_response_repeater_caller_secret_room_stored_secret_"
        "flood_path_return_or_direct_datagram_then_creation_outcome_exact_"
        "delays_and_coarse_dispatch_selection"
    )
    assert manifest["determinism"]["login_response_dispatch_matrix"] == (
        "seventeen_absent_repeater_room_flood_direct_encoded_path_creation_"
        "success_failure_room_push_delay_and_unknown_path_cases"
    )
    assert manifest["determinism"]["signed_advert_dispatch_recipe"] == (
        "mesh_post_decode_and_flood_filter_pass_complete_self_seen_gate_then_"
        "caller_supplied_"
        "identity_result_callback_and_canonical_flood_route_capacity_forward_"
        "policy_projection"
    )
    assert manifest["determinism"]["signed_advert_dispatch_matrix"] == (
        "twelve_valid_invalid_self_seen_incomplete_direct_zero_hop_flood_do_"
        "not_retransmit_forward_policy_path_capacity_and_one_byte_count_wrap_"
        "reject_cases_plus_nineteen_"
        "fail_closed_boundaries"
    )
    assert manifest["determinism"]["signed_advert_send_recipe"] == (
        "createadvert_length_then_pool_error_or_rtc_and_vendored_sign_then_"
        "caller_selected_flood_transport_flood_direct_or_zero_hop_ownership_"
        "and_queue_projection"
    )
    assert manifest["determinism"]["signed_advert_send_matrix"] == (
        "sixteen_oversize_pool_empty_flood_transport_direct_zero_hop_valid_"
        "encoded_path_invalid_route_retention_release_and_queue_cases"
    )
    assert manifest["determinism"]["independent_verifier_kat"] == (
        "RFC 8032 section 7.1 TEST 1 empty message"
    )
    assert manifest["determinism"]["canonical_s_regression"] == (
        "RFC 8032 TEST 1 signature scalar plus Ed25519 group order L"
    )
    assert manifest["determinism"]["identity_point_forgery_regression"] == (
        "identity public key plus identity R and zero S accepted by the pinned "
        "verifier before strict point validation"
    )
    assert manifest["determinism"]["public_group_recipe"] == (
        "basechatmesh_setchannel_padded16_or_full32_secret_sha256_hash_then_"
        "utils_aes128_ecb_zero_pad_and_two_byte_hmac_sha256"
    )
    assert manifest["determinism"]["dm_recipe"] == (
        "basechatmesh_timestamp_low_attempt_text_optional_full_attempt_then_"
        "mesh_hash_prefix_and_utils_aes128_hmac"
    )
    assert manifest["determinism"]["dm_attempt_matrix"] == "0_through_255"
    assert manifest["determinism"]["dm_exact_block_matrix"] == (
        "normal_text_lengths_11_through_155_step_16"
    )
    assert manifest["determinism"]["expected_ack_recipe"] == (
        "basechatmesh_timestamp_low_attempt_text_sender_public_key_sha256_"
        "truncated4_plus_optional_full_attempt_and_caller_rng_byte"
    )
    assert manifest["determinism"]["ack_path_recipe"] == (
        "mesh_path_return_encoded_path_ack_extra_then_utils_aes128_hmac"
    )
    assert manifest["determinism"]["ack_path_encodings"] == [
        "zero",
        "one_byte",
        "two_byte",
        "three_byte",
    ]
    assert manifest["determinism"]["path_return_recipe"] == (
        "mesh_createpathreturn_general_extra_or_caller_rng_uniqueness_then_"
        "utils_aes128_hmac"
    )
    assert manifest["determinism"]["path_return_encodings"] == [
        "zero",
        "one_byte",
        "two_byte_maximum",
        "three_byte_maximum",
    ]
    assert manifest["determinism"]["path_return_route_matrix"] == (
        "flood_transport_flood_direct_zero_hop_and_transport_zero_hop"
    )
    assert manifest["determinism"]["undefined_full_ack_body_boundary"] == (
        "normal_dm_5_plus_text_len_exact_aes_block_expected_hash_only"
    )
    assert manifest["vectors"] == {
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
    }
    assert manifest["capabilities"][0] == {
        "id": "packet_envelope",
        "status": "implemented",
        "owner": "pinned_upstream",
        "semantic": False,
    }
    assert manifest["capabilities"][1] == {
        "id": "advert_data_fields",
        "status": "implemented",
        "owner": "pinned_upstream",
        "semantic": True,
    }
    assert manifest["capabilities"][2] == {
        "id": "signed_advert_verification",
        "status": "implemented",
        "owner": "pinned_d1l_production_ed25519",
        "semantic": True,
        "scope": "message_layout_and_signature_verification_only_no_mesh_dispatch",
    }
    assert manifest["capabilities"][3] == {
        "id": "ed25519_point_validation",
        "status": "implemented",
        "owner": "pinned_d1l_production_ed25519",
        "semantic": True,
        "scope": (
            "canonical_encoding_and_low_order_rejection_for_advert_"
            "public_key_and_signature_r"
        ),
    }
    assert manifest["capabilities"][4] == {
        "id": "direct_flood_headers",
        "status": "implemented",
        "owner": "pinned_source_golden_vectors",
        "semantic": True,
        "scope": "non_trace_direct_flood_and_zero_hop_headers",
    }
    assert manifest["capabilities"][5] == {
        "id": "ack_frames",
        "status": "implemented",
        "owner": "pinned_source_golden_vectors",
        "semantic": True,
        "scope": "simple_and_multipart_payload_framing_only",
    }
    assert manifest["capabilities"][6] == {
        "id": "trace_source_frames",
        "status": "implemented",
        "owner": "pinned_source_golden_vectors",
        "semantic": True,
        "scope": "initial_outbound_direct_flags_zero_trace_framing_only",
    }
    assert manifest["capabilities"][7] == {
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
    }
    public_group = manifest["capabilities"][8]
    assert public_group["id"] == "public_group_packets"
    assert public_group["status"] == "implemented"
    assert public_group["owner"] == "pinned_upstream_utils_vendored_aes_host_sha"
    assert public_group["semantic"] is True
    assert public_group["implementation_receipt"]["id"] == (
        "RCPT-WP04-PUBLIC-GROUP-20260713"
    )
    assert public_group["implementation_receipt"]["vectors"] == {
        "roundtrip": 4,
        "invalid": 21,
        "crypto_adapter_kat": 3,
    }
    dm = manifest["capabilities"][9]
    assert dm["id"] == "dm_encrypt_decrypt"
    assert dm["status"] == "implemented"
    assert dm["owner"] == (
        "pinned_mesh_datagram_basechat_layout_vendored_aes_host_sha"
    )
    assert dm["semantic"] is True
    assert dm["implementation_receipt"]["id"] == (
        "RCPT-WP04-DM-CRYPTO-20260713"
    )
    assert dm["implementation_receipt"]["attempt_matrix"] == "0_through_255"
    assert dm["implementation_receipt"]["vectors"] == {
        "roundtrip": 268,
        "invalid": 29,
    }
    expected_ack = manifest["capabilities"][10]
    assert expected_ack["id"] == "expected_ack_hash_and_ack_path"
    assert expected_ack["status"] == "implemented"
    assert expected_ack["owner"] == (
        "pinned_basechat_ack_hash_mesh_ack_path_vendored_crypto"
    )
    assert expected_ack["semantic"] is True
    assert expected_ack["implementation_receipt"]["id"] == (
        "RCPT-WP04-EXPECTED-ACK-PATH-20260713"
    )
    assert expected_ack["implementation_receipt"]["return_path_encodings"] == [
        "zero",
        "one_byte",
        "two_byte",
        "three_byte",
    ]
    assert expected_ack["implementation_receipt"]["vectors"] == {
        "roundtrip": 4,
        "valid": 9,
        "invalid": 35,
    }
    assert expected_ack["implementation_receipt"][
        "undefined_upstream_boundary"
    ] == (
        "normal_dm_5_plus_text_len_exact_aes_block_has_deterministic_"
        "expected_hash_but_uninitialized_full_ack_body_byte"
    )
    path_return = manifest["capabilities"][13]
    assert path_return["id"] == "path_return_route_codes"
    assert path_return["status"] == "implemented"
    assert path_return["owner"] == (
        "pinned_mesh_path_return_utils_and_route_golden_vectors"
    )
    assert path_return["semantic"] is True
    assert path_return["implementation_receipt"]["id"] == (
        "RCPT-WP04-PATH-RETURN-ROUTE-CODES-20260713"
    )
    assert path_return["implementation_receipt"]["return_path_encodings"] == [
        "zero",
        "one_byte",
        "two_byte_maximum",
        "three_byte_maximum",
    ]
    assert path_return["implementation_receipt"]["route_matrix"] == [
        "flood",
        "transport_flood",
        "direct",
        "zero_hop",
        "transport_zero_hop",
    ]
    assert path_return["implementation_receipt"]["vectors"] == {
        "roundtrip": 6,
        "invalid": 35,
    }
    assert all(
        capability["status"] == "pending"
        for capability in manifest["capabilities"][7:]
        if capability["id"]
        not in {
            "public_group_packets",
            "dm_encrypt_decrypt",
            "expected_ack_hash_and_ack_path",
            "ack_dispatch_correlation_and_delivery",
            "path_return_route_codes",
            "signed_advert_packet_creation",
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
            "signed_advert_dispatch_transition_fixtures",
            "signed_advert_creation_send_orchestration_fixtures",
        }
    )
    assert [
        capability["id"] for capability in manifest["capabilities"][7:]
    ] == [
        "identity_signed_advert",
        "public_group_packets",
        "dm_encrypt_decrypt",
        "expected_ack_hash_and_ack_path",
        "ack_dispatch_correlation_and_delivery",
        "route_selection_and_forwarding",
        "path_return_route_codes",
        "trace_forwarding_and_path_discovery",
        "login_request_response_admin",
        "signed_advert_packet_creation",
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
        "signed_advert_dispatch_transition_fixtures",
        "signed_advert_creation_send_orchestration_fixtures",
    ]
    signed_advert_packets = manifest["capabilities"][-13]
    assert signed_advert_packets["id"] == "signed_advert_packet_creation"
    assert signed_advert_packets["status"] == "implemented"
    assert signed_advert_packets["owner"] == (
        "pinned_mesh_createadvert_vendored_ed25519"
    )
    assert signed_advert_packets["implementation_receipt"]["vectors"] == {
        "roundtrip": 3,
        "invalid": 23,
    }
    anonymous_login = manifest["capabilities"][-12]
    assert anonymous_login["id"] == "anonymous_login_request_packets"
    assert anonymous_login["status"] == "implemented"
    assert anonymous_login["owner"] == (
        "pinned_basechat_anon_datagram_vendored_crypto"
    )
    assert anonymous_login["implementation_receipt"]["vectors"] == {
        "roundtrip": 6,
        "invalid": 35,
    }
    regular_request_response = manifest["capabilities"][-11]
    assert regular_request_response["id"] == "regular_request_response_packets"
    assert regular_request_response["status"] == "implemented"
    assert regular_request_response["owner"] == (
        "pinned_mesh_regular_datagram_vendored_crypto"
    )
    assert regular_request_response["implementation_receipt"]["vectors"] == {
        "roundtrip": 6,
        "invalid": 30,
    }
    identity_exchange = manifest["capabilities"][-10]
    assert identity_exchange["id"] == "identity_shared_secret_derivation"
    assert identity_exchange["status"] == "implemented"
    assert identity_exchange["owner"] == (
        "pinned_ed25519_keypair_exchange_strict_peer_guard"
    )
    assert identity_exchange["implementation_receipt"]["vectors"] == {
        "roundtrip": 5,
        "invalid": 9,
    }
    canonical_login_response = manifest["capabilities"][-9]
    assert canonical_login_response["id"] == "canonical_login_response_packets"
    assert canonical_login_response["status"] == "implemented"
    assert canonical_login_response["owner"] == (
        "pinned_repeater_room_response_schema_vendored_crypto"
    )
    assert canonical_login_response["implementation_receipt"]["vectors"] == {
        "roundtrip": 10,
        "invalid": 34,
    }
    login_password_authorization = manifest["capabilities"][-8]
    assert login_password_authorization["id"] == (
        "login_password_authorization_fixtures"
    )
    assert login_password_authorization["status"] == "implemented"
    assert login_password_authorization["owner"] == (
        "pinned_repeater_room_password_rules"
    )
    assert login_password_authorization["implementation_receipt"]["vectors"] == {
        "valid": 16,
        "invalid": 17,
    }
    existing_acl_blank_login = manifest["capabilities"][-7]
    assert existing_acl_blank_login["id"] == (
        "existing_acl_blank_login_reuse_fixtures"
    )
    assert existing_acl_blank_login["status"] == "implemented"
    assert existing_acl_blank_login["owner"] == (
        "pinned_clientacl_lookup_repeater_room_reuse_rules"
    )
    assert existing_acl_blank_login["implementation_receipt"]["vectors"] == {
        "valid": 12,
        "invalid": 14,
    }
    authorized_login_acl_transition = manifest["capabilities"][-6]
    assert authorized_login_acl_transition["id"] == (
        "authorized_login_acl_transition_fixtures"
    )
    assert authorized_login_acl_transition["status"] == "implemented"
    assert authorized_login_acl_transition["owner"] == (
        "pinned_clientacl_putclient_repeater_room_login_mutation_rules"
    )
    assert authorized_login_acl_transition["implementation_receipt"][
        "vectors"
    ] == {"valid": 16, "invalid": 13}
    authenticated_request_replay = manifest["capabilities"][-5]
    assert authenticated_request_replay["id"] == (
        "authenticated_request_replay_transition_fixtures"
    )
    assert authenticated_request_replay["status"] == "implemented"
    assert authenticated_request_replay["owner"] == (
        "pinned_repeater_room_authenticated_request_replay_rules"
    )
    assert authenticated_request_replay["implementation_receipt"][
        "vectors"
    ] == {"valid": 15, "invalid": 12}
    authenticated_text_transition = manifest["capabilities"][-4]
    assert authenticated_text_transition["id"] == (
        "authenticated_text_replay_response_session_fixtures"
    )
    assert authenticated_text_transition["status"] == "implemented"
    assert authenticated_text_transition["owner"] == (
        "pinned_repeater_room_authenticated_text_orchestration_rules"
    )
    assert authenticated_text_transition["implementation_receipt"][
        "vectors"
    ] == {"valid": 20, "invalid": 17}
    login_response_dispatch = manifest["capabilities"][-3]
    assert login_response_dispatch["id"] == (
        "login_response_creation_dispatch_orchestration_fixtures"
    )
    assert login_response_dispatch["status"] == "implemented"
    assert login_response_dispatch["owner"] == (
        "pinned_repeater_room_login_response_dispatch_rules"
    )
    assert login_response_dispatch["implementation_receipt"]["vectors"] == {
        "valid": 17,
        "invalid": 14,
    }
    signed_advert_dispatch = manifest["capabilities"][-2]
    assert signed_advert_dispatch["id"] == (
        "signed_advert_dispatch_transition_fixtures"
    )
    assert signed_advert_dispatch["status"] == "implemented"
    assert signed_advert_dispatch["owner"] == (
        "pinned_mesh_signed_advert_gate_and_route_rules"
    )
    assert signed_advert_dispatch["implementation_receipt"][
        "upstream_identity_parity_proven"
    ] is False
    assert signed_advert_dispatch["implementation_receipt"][
        "d1l_path_count_wrap_reject_diverges_from_upstream"
    ] is True
    assert signed_advert_dispatch["implementation_receipt"]["vectors"] == {
        "valid": 12,
        "invalid": 19,
    }
    signed_advert_send = manifest["capabilities"][-1]
    assert signed_advert_send["id"] == (
        "signed_advert_creation_send_orchestration_fixtures"
    )
    assert signed_advert_send["status"] == "implemented"
    assert signed_advert_send["owner"] == (
        "pinned_mesh_dispatcher_advert_send_ordering_rules"
    )
    assert signed_advert_send["implementation_receipt"][
        "keypair_consistency_proven"
    ] is False
    assert signed_advert_send["implementation_receipt"]["vectors"] == {
        "valid": 16,
        "invalid": 9,
    }
    pending = {
        capability["id"]: capability
        for capability in manifest["capabilities"]
        if capability["status"] == "pending"
    }
    implemented = {
        capability["id"]: capability
        for capability in manifest["capabilities"]
        if capability["status"] == "implemented"
    }
    ack = implemented["ack_dispatch_correlation_and_delivery"]
    assert ack["implementation_receipt"] == {
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
    }
    assert pending["trace_forwarding_and_path_discovery"]["blocked_by"] == (
        "deterministic_identity_mesh_tables_radio_snr_and_clock_fixtures"
    )
    assert pending["trace_forwarding_and_path_discovery"]["owner"] == (
        "d1l_production_contact_trace"
    )
    assert pending["trace_forwarding_and_path_discovery"][
        "implemented_prerequisites"
    ] == [
        "trace_source_frames",
        "fingerprint_only_current_boot_proven_contact_loop_dispatch",
    ]
    assert pending["trace_forwarding_and_path_discovery"][
        "production_receipt"
    ] == {
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
    }
    assert pending["route_selection_and_forwarding"]["blocked_by"] == (
        "deterministic_mesh_dispatch_packet_manager_tables_radio_rng_and_clock_fixtures"
    )
    assert pending["login_request_response_admin"]["blocked_by"] == (
        "exact_actions_build_runtime_and_physical_delivery_evidence"
    )
    assert pending["login_request_response_admin"]["implemented_prerequisites"] == [
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
    ]
    assert pending["login_request_response_admin"]["host_only_receipt"] == (
        "RCPT-WP04-HOST-ADMIN-SESSION-20260714"
    )
    assert pending["login_request_response_admin"]["production_receipt"] == {
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
    }
    assert pending["login_request_response_admin"]["queue_fairness_receipt"] == {
        "id": "RCPT-WP06-QUEUE-FAIRNESS-20260716",
        "status": (
            "implemented_host_only_pending_actions_hardware_rf_and_wp06_closure"
        ),
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
            "terminal_recovery_precedence_four_radio_event_and_two_priority_"
            "command_burst_bounds_synchronous_request_cancellation_"
            "credential_wipe_owner_recursion_guard_and_truthful_saturation_"
            "fairness_maintenance_telemetry_no_actions_hardware_rf_wp06_or_"
            "release_closure"
        ),
    }

    for relative, expected in {
        **manifest["upstream"]["sources"],
        **manifest["production_binding_sources"],
        **manifest["vendored_crypto_sources"],
        **manifest["oracle_sources"],
    }.items():
        assert canonical_lf_sha256(ROOT / relative) == expected


def test_oracle_coverage_manifest_accounts_for_every_required_surface():
    wire_manifest = conformance.load_manifest()
    oracle_manifest = conformance.load_oracle_manifest()
    coverage = conformance.load_oracle_coverage_manifest(
        oracle_manifest, wire_manifest
    )
    summary = conformance.summarize_oracle_coverage(coverage, oracle_manifest)

    assert coverage == json.loads(COVERAGE_MANIFEST.read_text(encoding="utf-8"))
    assert coverage["host_only_evidence"] == conformance.EXPECTED_HOST_ONLY_EVIDENCE
    assert summary["validated"] is True
    assert summary["wp04_closure_eligible"] is False
    assert summary["closure_ready"] is False
    assert summary["unsupported_closure_rejected"] is True
    assert summary["required_surface_count"] == 9
    assert summary["implemented_surface_count"] == 6
    assert summary["partial_surface_count"] == 3
    assert summary["blocked_surface_count"] == 0
    assert summary["local_packet_type_count"] == 8
    assert summary["wire_vector_covered_packet_type_count"] == 8
    assert summary["unknown_packet_type_policy"] == "fail_closed"
    assert len(summary["blocker_receipts"]) == 4
    assert len(summary["unresolved_capabilities"]) == 4
    assert len(summary["local_packet_types"]) == 8
    identity_receipt = next(
        item
        for item in summary["blocker_receipts"]
        if item["capability"] == "identity_signed_advert"
    )["receipt"]
    assert identity_receipt["id"] == "BLK-WP04-IDENTITY-DISPATCH-20260713"
    assert identity_receipt["blocks_execution"] is False
    assert identity_receipt["unblocked_slice"] == (
        "signed_advert_packet_creation_dispatch_and_send_orchestration"
    )


@pytest.mark.parametrize(
    ("declaration", "error"),
    [
        (
            "#define D1L_MESHCORE_PAYLOAD_UNREVIEWED 0x0CU",
            "local packet type changed without a coverage vector",
        ),
        (
            "#define D1L_MESHCORE_PAYLOAD_UNREVIEWED 0x0CU // unreviewed",
            "local MeshCore packet type value syntax is unsupported",
        ),
        (
            "#define D1L_MESHCORE_PAYLOAD_UNREVIEWED (0x0CU)",
            "local MeshCore packet type value syntax is unsupported",
        ),
        (
            "#define D1L_MESHCORE_PAYLOAD_VER_UNREVIEWED 0x0CU",
            "local packet type changed without a coverage vector",
        ),
    ],
)
def test_local_packet_type_addition_without_vector_fails_closed(
    tmp_path, declaration, error
):
    wire_manifest = conformance.load_manifest()
    oracle_manifest = conformance.load_oracle_manifest()
    header = ROOT / "main" / "mesh" / "meshcore_wire.h"
    changed_header = tmp_path / "meshcore_wire.h"
    changed_header.write_text(
        header.read_text(encoding="utf-8")
        + f"\n{declaration}\n",
        encoding="utf-8",
    )

    with pytest.raises(
        conformance.GateFailure,
        match=error,
    ):
        conformance.load_oracle_coverage_manifest(
            oracle_manifest,
            wire_manifest,
            packet_type_source=changed_header,
        )


def test_duplicate_local_packet_type_code_fails_closed(tmp_path):
    wire_manifest = conformance.load_manifest()
    oracle_manifest = conformance.load_oracle_manifest()
    header = ROOT / "main" / "mesh" / "meshcore_wire.h"
    changed_header = tmp_path / "meshcore_wire.h"
    changed_header.write_text(
        header.read_text(encoding="utf-8")
        + "\n#define D1L_MESHCORE_PAYLOAD_UNREVIEWED 0x0AU\n",
        encoding="utf-8",
    )

    with pytest.raises(
        conformance.GateFailure,
        match="local MeshCore packet type codes must be unique",
    ):
        conformance.load_oracle_coverage_manifest(
            oracle_manifest,
            wire_manifest,
            packet_type_source=changed_header,
        )


@pytest.mark.parametrize(
    ("mutation", "error"),
    [
        ("missing_surface", "oracle required-surface mapping drifted"),
        ("unknown_capability", "oracle required-surface mapping drifted"),
        ("valid_surface_swap", "oracle required-surface mapping drifted"),
        (
            "valid_packet_swap",
            "local packet type semantic coverage mapping drifted",
        ),
    ],
)
def test_oracle_coverage_registry_mutations_fail_closed(tmp_path, mutation, error):
    wire_manifest = conformance.load_manifest()
    oracle_manifest = conformance.load_oracle_manifest()
    coverage = json.loads(COVERAGE_MANIFEST.read_text(encoding="utf-8"))
    if mutation == "missing_surface":
        coverage["required_surfaces"].pop()
    elif mutation == "unknown_capability":
        coverage["required_surfaces"][0]["capabilities"].append(
            "unreviewed_capability"
        )
    elif mutation == "valid_surface_swap":
        public_capabilities = coverage["required_surfaces"][1]["capabilities"]
        dm_capabilities = coverage["required_surfaces"][2]["capabilities"]
        coverage["required_surfaces"][1]["capabilities"] = dm_capabilities
        coverage["required_surfaces"][2]["capabilities"] = public_capabilities
    else:
        packet_types = coverage["local_packet_type_policy"]["packet_types"]
        text_capabilities = packet_types[0]["semantic_capabilities"]
        group_capabilities = packet_types[3]["semantic_capabilities"]
        packet_types[0]["semantic_capabilities"] = group_capabilities
        packet_types[3]["semantic_capabilities"] = text_capabilities
    changed_coverage = tmp_path / "coverage_manifest.json"
    changed_coverage.write_text(
        json.dumps(coverage, indent=2) + "\n", encoding="utf-8"
    )

    with pytest.raises(conformance.GateFailure, match=error):
        conformance.load_oracle_coverage_manifest(
            oracle_manifest,
            wire_manifest,
            coverage_manifest_path=changed_coverage,
        )


def test_oracle_c_abi_wraps_pinned_protocol_helpers_and_production_s_guard():
    header = (ORACLE_ROOT / "meshcore_oracle.h").read_text(encoding="utf-8")
    adapter = (ORACLE_ROOT / "meshcore_oracle.cpp").read_text(encoding="utf-8")
    service = (ROOT / "main" / "mesh" / "meshcore_service.c").read_text(
        encoding="utf-8"
    )
    canonical_guard = (
        ROOT / "main" / "mesh" / "ed25519_canonical.h"
    ).read_text(encoding="utf-8")

    assert '"e8d3c53ba1ea863937081cd0caad759b832f3028"' in header
    assert 'extern "C"' in header
    assert "mesh::Packet upstream" in adapter
    assert "upstream.readFrom" in adapter
    assert "upstream.writeTo" in adapter
    assert "structurally_safe_to_read" in adapter
    assert "structurally_safe_to_write" in adapter
    assert "AdvertDataBuilder upstream" in adapter
    assert "AdvertDataParser upstream" in adapter
    assert "advert_layout_is_canonical" in adapter
    assert "d1l_meshcore_oracle_advert_data_decode" in header
    assert "d1l_meshcore_oracle_advert_data_encode" in header
    assert "d1l_meshcore_oracle_verify_signed_advert" in header
    assert "d1l_meshcore_oracle_create_signed_advert_packet" in header
    assert "d1l_meshcore_oracle_parse_signed_advert_packet" in header
    assert "d1l_meshcore_oracle_identity_shared_secret_from_seed" in header
    assert "d1l_meshcore_oracle_identity_exchange_t" in header
    assert "d1l_meshcore_oracle_create_login_request_packet" in header
    assert "d1l_meshcore_oracle_parse_login_request_packet" in header
    assert "d1l_meshcore_oracle_create_request_response_packet" in header
    assert "d1l_meshcore_oracle_parse_request_response_packet" in header
    assert "d1l_meshcore_oracle_apply_login_response_dispatch_transition" in header
    assert "d1l_meshcore_oracle_apply_signed_advert_dispatch_transition" in header
    assert "d1l_meshcore_oracle_apply_signed_advert_send_transition" in header
    assert "D1L_MESHCORE_ORACLE_MAX_REQUEST_RESPONSE_PLAINTEXT_BYTES" in header
    assert "d1l_meshcore_oracle_group_channel_hash" in header
    assert "d1l_meshcore_oracle_create_group_packet" in header
    assert "d1l_meshcore_oracle_parse_group_packet" in header
    assert "d1l_meshcore_oracle_create_dm_packet" in header
    assert "d1l_meshcore_oracle_parse_dm_packet" in header
    assert "D1L_MESHCORE_ORACLE_MAX_DM_TEXT_BYTES" in header
    assert "d1l_meshcore_oracle_dm_expected_ack_hash" in header
    assert "d1l_meshcore_oracle_dm_expected_ack" in header
    assert "d1l_meshcore_oracle_create_dm_ack_path_packet" in header
    assert "d1l_meshcore_oracle_parse_dm_ack_path_packet" in header
    assert "D1L_MESHCORE_ORACLE_DM_ACK_BYTES" in header
    assert "d1l_meshcore_oracle_create_path_return_extra_packet" in header
    assert "d1l_meshcore_oracle_create_path_return_unique_packet" in header
    assert "d1l_meshcore_oracle_parse_path_return_extra_packet" in header
    assert "d1l_meshcore_oracle_parse_path_return_unique_packet" in header
    assert "D1L_MESHCORE_ORACLE_MAX_PATH_RETURN_EXTRA_BYTES" in header
    assert "ed25519_verify" in adapter
    assert "ed25519_create_keypair" in adapter
    assert "ed25519_sign" in adapter
    assert "ed25519_key_exchange" in adapter
    assert "secure_zero" in adapter
    assert '#include "mesh/ed25519_canonical.h"' in adapter
    assert '#include "mesh/ed25519_canonical.h"' in service
    assert "d1l_ed25519_signature_s_is_canonical(signature)" in adapter
    assert "d1l_ed25519_signature_s_is_canonical(signature)" in service
    assert "d1l_ed25519_encoded_point_is_strict(public_key)" in adapter
    assert "d1l_ed25519_encoded_point_is_strict(pub_key)" in service
    assert "d1l_ed25519_encoded_point_is_strict(signature)" in adapter
    assert "d1l_ed25519_encoded_point_is_strict(signature)" in service
    assert "ge_frombytes_negate_vartime" in canonical_guard
    assert "ge_p3_dbl" in canonical_guard
    assert "multiplied_by_cofactor" in canonical_guard
    assert adapter.index("d1l_ed25519_signature_s_is_canonical(signature)") < (
        adapter.index("ed25519_verify(signature")
    )
    assert service.index("d1l_ed25519_signature_s_is_canonical(signature)") < (
        service.index("ed25519_verify(signature")
    )
    assert "D1L_ADVERT_DATA_MAX_LEN" in adapter
    assert "S >= L" in canonical_guard
    assert "d1l_meshcore_oracle_prepare_flood" in header
    assert "d1l_meshcore_oracle_prepare_direct" in header
    assert "d1l_meshcore_oracle_prepare_zero_hop" in header
    assert "d1l_meshcore_oracle_create_ack" in header
    assert "d1l_meshcore_oracle_create_multi_ack" in header
    assert "d1l_meshcore_oracle_parse_ack" in header
    assert "d1l_meshcore_oracle_create_trace" in header
    assert "d1l_meshcore_oracle_prepare_trace_direct" in header
    assert "d1l_meshcore_oracle_parse_trace_source" in header
    assert "mesh::Packet::copyPath" in adapter
    assert "PAYLOAD_TYPE_TRACE" in adapter
    assert "Identity" not in adapter
    assert "Dispatcher" not in adapter


def test_conformance_plan_builds_the_oracle_as_a_separate_target():
    commands = conformance.command_plan("clang", "clang++")
    flattened = [" ".join(command) for command in commands]

    assert any("meshcore_oracle.cpp" in command for command in flattened)
    assert any("meshcore_admin_session_fixture.cpp" in command for command in flattened)
    assert any("meshcore_admin_session_fixture.o" in command for command in flattened)
    assert any("meshcore_oracle_vectors.cpp" in command for command in flattened)
    assert any("AdvertDataHelpers.cpp" in command for command in flattened)
    assert any("meshcore_advert_data.o" in command for command in flattened)
    assert any("soft-se\\aes.c" in command or "soft-se/aes.c" in command for command in flattened)
    assert any("Utils.cpp" in command for command in flattened)
    assert any("meshcore_oracle_aes.o" in command for command in flattened)
    assert any("meshcore_oracle_utils.o" in command for command in flattened)
    packet_commands = [
        command.replace("\\", "/")
        for command in flattened
        if "src\\Packet.cpp" in command or "src/Packet.cpp" in command
    ]
    assert len(packet_commands) == 2
    assert any(
        "meshcore_conformance/stubs" in command
        and "meshcore_packet.o" in command
        for command in packet_commands
    )
    assert any(
        "meshcore_oracle/stubs" in command
        and "meshcore_oracle_packet.o" in command
        for command in packet_commands
    )
    for source in conformance.ED25519_ORACLE_SOURCES:
        source_path = str(source)
        source_commands = [
            command for command in flattened if source_path in command
        ]
        assert len(source_commands) == 1
        assert "-fsanitize=address,undefined" in source_commands[0]
        assert f"meshcore_ed25519_{source.stem}.o" in source_commands[0]
        assert "-fno-sanitize=" not in source_commands[0]
    assert not any("-fno-sanitize=" in command for command in flattened)
    oracle_link = next(
        command
        for command in flattened
        if command.endswith("meshcore_oracle_vectors")
    )
    assert "meshcore_oracle_packet.o" in oracle_link
    assert "meshcore_packet.o" not in oracle_link.replace(
        "meshcore_oracle_packet.o", ""
    )
    for source in conformance.ED25519_ORACLE_SOURCES:
        assert f"meshcore_ed25519_{source.stem}.o" in oracle_link
    assert any(command.endswith("meshcore_oracle_vectors") for command in flattened)
    assert all("main/mesh/meshcore_service" not in command for command in flattened)


def test_dry_run_writes_a_versioned_fail_closed_oracle_artifact(tmp_path):
    output = tmp_path / "meshcore_conformance.json"
    oracle_output = tmp_path / f"meshcore_oracle_manifest_{git_head()}.json"
    env = os.environ.copy()
    env.pop("D1L_MESHCORE_CONFORMANCE_CI", None)
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--commit",
            git_head(),
            "--out",
            str(output),
            "--oracle-out",
            str(oracle_output),
            "--dry-run",
        ],
        cwd=ROOT,
        env=env,
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
    report = json.loads(output.read_text(encoding="utf-8"))
    artifact = json.loads(oracle_output.read_text(encoding="utf-8"))
    assert report["oracle"]["artifact_path"] == str(oracle_output)
    assert report["oracle"]["source_verification"]["verified"] is True
    assert report["oracle"]["result"] is None
    assert report["sanitizer_policy"] == conformance.ED25519_SANITIZER_POLICY
    assert report["full_ubsan_clean"] is True
    assert report["sanitizer_policy_passed"] is None
    assert artifact["artifact_type"] == "meshcore_oracle_manifest"
    assert artifact["status"] == "dry_run"
    assert artifact["passed"] is False
    assert artifact["execution_complete"] is False
    assert artifact["coverage_boundary"] == BOUNDARY
    assert artifact["wp04_closure_eligible"] is False
    assert artifact["closure_ready"] is False
    assert artifact["wp04_acceptance_ready"] is False
    assert artifact["sanitizer_policy"] == conformance.ED25519_SANITIZER_POLICY
    assert artifact["full_ubsan_clean"] is True
    assert artifact["sanitizer_policy_passed"] is None
    assert artifact["corpus_version"] == 24
    assert artifact["coverage_policy"]["validated"] is True
    assert artifact["coverage_policy"]["unsupported_closure_rejected"] is True
    assert artifact["coverage_policy"]["local_packet_type_count"] == 8
    assert artifact["repository_commit"] == git_head()
    assert artifact["upstream_commit"] == UPSTREAM_COMMIT
    assert artifact["oracle_result"] is None
    assert len(artifact["pending_capabilities"]) == 4
    assert "packet_envelope" not in artifact["pending_capabilities"]
    assert "advert_data_fields" not in artifact["pending_capabilities"]
    assert "signed_advert_packet_creation" not in artifact["pending_capabilities"]
    assert "anonymous_login_request_packets" not in artifact["pending_capabilities"]
    assert "regular_request_response_packets" not in artifact["pending_capabilities"]
    assert "identity_shared_secret_derivation" not in artifact["pending_capabilities"]
    assert "canonical_login_response_packets" not in artifact["pending_capabilities"]
    assert (
        "login_password_authorization_fixtures"
        not in artifact["pending_capabilities"]
    )
    assert (
        "existing_acl_blank_login_reuse_fixtures"
        not in artifact["pending_capabilities"]
    )
    assert (
        "authorized_login_acl_transition_fixtures"
        not in artifact["pending_capabilities"]
    )
    assert (
        "authenticated_request_replay_transition_fixtures"
        not in artifact["pending_capabilities"]
    )
    assert (
        "authenticated_text_replay_response_session_fixtures"
        not in artifact["pending_capabilities"]
    )
    assert (
        "login_response_creation_dispatch_orchestration_fixtures"
        not in artifact["pending_capabilities"]
    )
    assert (
        "signed_advert_dispatch_transition_fixtures"
        not in artifact["pending_capabilities"]
    )
    assert (
        "signed_advert_creation_send_orchestration_fixtures"
        not in artifact["pending_capabilities"]
    )
    assert "signed_advert_verification" not in artifact["pending_capabilities"]
    assert "ed25519_point_validation" not in artifact["pending_capabilities"]
    assert "public_group_packets" not in artifact["pending_capabilities"]
    assert "dm_encrypt_decrypt" not in artifact["pending_capabilities"]
    assert "expected_ack_hash_and_ack_path" not in artifact["pending_capabilities"]
    assert "path_return_route_codes" not in artifact["pending_capabilities"]
    assert "direct_flood_headers" not in artifact["pending_capabilities"]
    assert "ack_frames" not in artifact["pending_capabilities"]
    assert "trace_source_frames" not in artifact["pending_capabilities"]


def test_oracle_vectors_compile_and_run_deterministically(tmp_path):
    compiler_pair = next(
        (
            (cxx, cc)
            for cxx, cc in (
                (shutil.which("g++"), shutil.which("gcc")),
                (shutil.which("clang++"), shutil.which("clang")),
            )
            if cxx is not None and cc is not None
        ),
        None,
    )
    if compiler_pair is None:
        pytest.skip("no matching host C/C++ compiler pair available")
    compiler, c_compiler = compiler_pair
    executable = tmp_path / (
        "meshcore_oracle_vectors.exe" if os.name == "nt" else "meshcore_oracle_vectors"
    )
    aes_object = tmp_path / "meshcore_oracle_aes.o"
    ed25519_objects = []
    subprocess.run(
        [
            c_compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "main"),
            "-I",
            str(conformance.ED25519_ROOT),
            "-x",
            "c",
            "-fsyntax-only",
            "-",
        ],
        cwd=ROOT,
        input=(
            '#include "mesh/ed25519_canonical.h"\n'
            "int main(void) { unsigned char signature[64] = {0}; "
            "return d1l_ed25519_signature_s_is_canonical(signature) ? 0 : 1; }\n"
        ),
        check=True,
        capture_output=True,
        text=True,
    )
    for source in conformance.ED25519_ORACLE_SOURCES:
        object_path = tmp_path / f"meshcore_ed25519_{source.stem}.o"
        subprocess.run(
            [
                c_compiler,
                "-std=c11",
                "-O1",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(conformance.ED25519_ROOT),
                "-c",
                str(source),
                "-o",
                str(object_path),
            ],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        ed25519_objects.append(str(object_path))
    subprocess.run(
        [
            c_compiler,
            "-std=c11",
            "-O1",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D__CORTEX_M=0",
            "-DAES_DEC_PREKEYED=1",
            "-I",
            str(conformance.ORACLE_AES_ROOT),
            "-c",
            str(conformance.ORACLE_AES_SOURCE),
            "-o",
            str(aes_object),
        ],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    command = [
        compiler,
        "-std=c++17",
        "-O1",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ORACLE_ROOT),
        "-I",
        str(ORACLE_ROOT / "stubs"),
        "-I",
        str(ROOT / "main"),
        "-I",
        str(conformance.ED25519_ROOT),
        "-isystem",
        str(ROOT / "third_party" / "MeshCore" / "src"),
        str(ROOT / "third_party" / "MeshCore" / "src" / "Packet.cpp"),
        str(
            ROOT
            / "third_party"
            / "MeshCore"
            / "src"
            / "helpers"
            / "AdvertDataHelpers.cpp"
        ),
        str(conformance.ORACLE_UTILS_SOURCE),
        str(ORACLE_ROOT / "meshcore_oracle.cpp"),
        str(ORACLE_ROOT / "meshcore_admin_session_fixture.cpp"),
        str(ORACLE_ROOT / "meshcore_oracle_vectors.cpp"),
        str(aes_object),
        *ed25519_objects,
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)

    first = subprocess.run(
        [str(executable)], cwd=ROOT, check=True, capture_output=True, text=True
    )
    second = subprocess.run(
        [str(executable)], cwd=ROOT, check=True, capture_output=True, text=True
    )
    assert first.stdout == second.stdout
    assert first.stderr == second.stderr == ""
    actual = json.loads(first.stdout)
    admin_session = actual.pop("admin_session_fixture")
    conformance.validate_host_admin_session_receipt(admin_session)
    assert admin_session["artifact_type"] == "meshcore_host_admin_session_fixture"
    assert admin_session["receipt_id"] == "RCPT-WP04-HOST-ADMIN-SESSION-20260714"
    assert admin_session["status"] == "pass"
    assert admin_session["host_only"] is True
    assert admin_session["production_runtime_proven"] is False
    assert admin_session["wp18_closure_eligible"] is False
    assert admin_session["rf_closure_eligible"] is False
    assert admin_session["ui_closure_eligible"] is False
    assert admin_session["hardware_closure_eligible"] is False
    assert admin_session["upstream_commit"] == UPSTREAM_COMMIT
    assert admin_session["invariants"] == {
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
    }
    assert admin_session["canonical_bytes"] == {
        "get_status_request_hex": "050302010100000000a1b2c3d4",
        "login_response_hex": "88776655000001031020304002",
        "get_status_response_hex": (
            "05030201740e07008bffadff0403020114131211242322213433323144434241"
            "5453525164636261747372718281ecff9291a2a1b4b3b2b1c4c3c2c1"
        ),
    }
    assert admin_session["repeater_stats"] == {
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
    assert admin_session["positive_checks"] == 6
    assert admin_session["negative_checks"] == 16
    assert [entry["id"] for entry in admin_session["negative_matrix"]] == [
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
    assert all(entry["rejected"] is True for entry in admin_session["negative_matrix"])
    assert admin_session["zeroization"] == {
        "timeout_pending_state": True,
        "timeout_pending_login_state": True,
        "explicit_logout_client_server_session": True,
    }
    assert len(admin_session["transcript"]) == 11
    assert actual == {
        "passed": True,
        "coverage_boundary": BOUNDARY,
        "wp04_closure_eligible": False,
        "abi_version": 2,
        "upstream_commit": UPSTREAM_COMMIT,
        "vectors": {
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
        },
        "capabilities": {
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
        },
        "failures": 0,
    }
