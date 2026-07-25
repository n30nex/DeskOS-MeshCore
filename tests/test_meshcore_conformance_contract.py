import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import copy
from pathlib import Path

import pytest

from scripts import meshcore_conformance_d1l as conformance
from scripts import meshcore_remaining_fuzz_d1l as remaining_fuzz
from tests.meshcore_conformance_fixture import (
    completed_report,
    completed_signed_advert_runtime_report,
)


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "meshcore_conformance_d1l.py"
MANIFEST = ROOT / "tests" / "meshcore_conformance" / "manifest.json"
CORPUS = ROOT / "tests" / "meshcore_conformance" / "corpus.json"
ADVERT_CORPUS = ROOT / "tests" / "meshcore_conformance" / "advert_corpus.json"
WP05_MATRIX = ROOT / "tests" / "meshcore_oracle" / "wp05_semantic_matrix.json"


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def canonical_lf_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def dry_run_environment() -> dict[str, str]:
    env = os.environ.copy()
    env.pop("D1L_MESHCORE_CONFORMANCE_CI", None)
    return env


def run_dependency_outputs(cc: str) -> list[str]:
    outputs: list[str] = []
    for command in conformance.production_semantic_dependency_command_plan(cc):
        result = subprocess.run(
            command,
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, (
            f"dependency scan failed ({result.returncode}): {command!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
        outputs.append(result.stdout)
    return outputs


def dependency_test_compiler() -> str | None:
    """Match the portable native-suite compiler choice used by host tests."""

    return os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")


def test_conformance_manifest_pins_exact_upstream_and_fixed_scope():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert manifest["coverage_boundary"] == "wire_envelope_only"
    assert manifest["issue_65_closure_eligible"] is False
    assert manifest["upstream"]["commit"] == "e8d3c53ba1ea863937081cd0caad759b832f3028"
    assert manifest["vectors"]["local_to_upstream"] == 504
    assert manifest["vectors"]["upstream_to_local"] == 504
    assert manifest["vectors"]["total"] == 1008
    assert [item["name"] for item in manifest["vector_matrix"]["payload_types"]] == [
        "TXT", "ACK", "ADVERT", "GRP_TXT", "PATH", "TRACE", "MULTIPART"
    ]
    assert len(manifest["vector_matrix"]["route_types"]) == 4
    assert len(manifest["vector_matrix"]["path_cases"]) == 9
    assert manifest["vector_matrix"]["payload_lengths"] == [1, 184]
    assert manifest["payload_version_gate"] == {
        "permissive_envelope_versions": [0, 1, 2, 3],
        "accepted_v1_versions": [0],
        "rejected_future_versions": [1, 2, 3],
        "preserve_output_on_reject": True,
    }
    assert manifest["fuzz_corpus"]["manifest"] == "tests/meshcore_conformance/corpus.json"
    assert manifest["fuzz_corpus"]["seed_count"] == 5
    assert manifest["fuzz_corpus"]["sha256"] == canonical_lf_sha256(CORPUS)
    assert manifest["advert_fuzz_corpus"] == {
        "manifest": "tests/meshcore_conformance/advert_corpus.json",
        "sha256": canonical_lf_sha256(ADVERT_CORPUS),
        "seed_count": 8,
    }
    assert manifest["fuzz"] == {
        "engine": "libFuzzer",
        "runs": 100000,
        "seed": 13746277,
        "max_input_bytes": 255,
        "sanitizers": ["address", "undefined"],
    }
    assert manifest["advert_fuzz"] == {
        "engine": "libFuzzer",
        "runs": 100000,
        "seed": 13746277,
        "max_input_bytes": 33,
        "sanitizers": ["address", "undefined"],
        "target": "local_advert_parser",
        "production_sources": [
            "main/mesh/advert_data.c",
            "main/mesh/advert_data.h",
        ],
        "invalid_output_policy": "canonical_zeroed_with_type_N",
    }
    for relative, expected in manifest["upstream"]["sources"].items():
        actual = canonical_lf_sha256(ROOT / "third_party" / "MeshCore" / relative)
        assert actual == expected


def test_corpus_manifest_is_deterministic_and_hash_checked():
    corpus = json.loads(CORPUS.read_text(encoding="utf-8"))

    assert corpus["encoding"] == "hex"
    assert corpus["coverage_boundary"] == "local_wire_decoder_only"
    assert len(corpus["seeds"]) >= 5
    assert len({seed["name"] for seed in corpus["seeds"]}) == len(corpus["seeds"])
    for seed in corpus["seeds"]:
        payload = bytes.fromhex(seed["hex"])
        assert payload
        assert len(payload) <= 255
        assert hashlib.sha256(payload).hexdigest() == seed["sha256"]


def test_advert_corpus_is_deterministic_and_covers_reject_boundaries():
    corpus = json.loads(ADVERT_CORPUS.read_text(encoding="utf-8"))

    assert corpus["encoding"] == "hex"
    assert corpus["coverage_boundary"] == "local_advert_parser"
    assert len(corpus["seeds"]) == 8
    seeds = {seed["name"]: seed for seed in corpus["seeds"]}
    assert {"truncated_location", "latitude_out_of_range", "oversize"} <= set(
        seeds
    )
    for seed in seeds.values():
        payload = bytes.fromhex(seed["hex"])
        assert payload
        assert len(payload) <= 33
        assert hashlib.sha256(payload).hexdigest() == seed["sha256"]


def test_wp05_semantic_matrix_accounts_for_declared_host_surface_fail_closed():
    oracle = conformance.load_oracle_manifest()
    matrix = conformance.load_wp05_semantic_matrix(oracle)
    summary = conformance.summarize_wp05_semantic_matrix(matrix, oracle)

    assert summary["sha256"] == canonical_lf_sha256(WP05_MATRIX)
    assert summary["closure_ready"] is False
    assert summary["full_semantic_matrix_covered"] is False
    assert (
        summary["implemented_requirement_count"],
        summary["partial_requirement_count"],
        summary["missing_requirement_count"],
    ) == (13, 3, 0)
    assert (
        summary["implemented_fuzz_target_count"],
        summary["partial_fuzz_target_count"],
        summary["missing_fuzz_target_count"],
    ) == (8, 0, 0)
    assert summary["oracle_semantic_vectors"] == 915
    assert summary["unique_referenced_oracle_semantic_vectors"] == 890
    assert summary["production_suite_count"] == 11
    assert summary["production_scenario_count"] == 75
    assert summary["companion_upstream_suite_count"] == 1
    assert summary["companion_upstream_case_count"] == 10
    assert [suite["id"] for suite in summary["production_host_suites"]] == [
        "admin_dispatch",
        "runtime_queue_fairness",
        "contact_trace",
        "legacy_protocol_timestamp_migration",
        "time_service_migration_integration",
        "text_admission",
        "ack_delivery_correlation",
        "advert_replay_admission",
        "packet_hash_and_advert_dedupe",
        "lifetime_boundaries",
        "channel_and_peer_rx_admission",
    ]
    assert summary["companion_upstream_suites"] == [
        {
            "id": "pinned_signed_advert_runtime",
            "artifact_type": "d1l_meshcore_signed_advert_semantic_runtime",
            "capability": "identity_signed_advert_semantic_runtime",
            "case_ids": [
                *conformance.SIGNED_ADVERT_REPLAY_OUTCOME_KEYS,
                *conformance.SIGNED_ADVERT_PACKET_HASH_OUTCOME_KEYS,
            ],
            "identical_wire_hash_case": "identical_wire_hash_suppressed",
            "closure_ready": False,
        }
    ]
    source_verification = conformance.verify_wp05_semantic_sources(matrix)
    assert source_verification["pins_verified"] is True
    assert source_verification["verified"] is False
    assert source_verification["dependency_closure_verified"] is False

    cc = dependency_test_compiler()
    assert cc is not None
    dependency_outputs = run_dependency_outputs(cc)
    dependency_verification = conformance.verify_wp05_semantic_dependencies(
        matrix,
        dependency_outputs,
        cc,
    )
    conformance.finalize_wp05_semantic_source_verification(
        source_verification,
        dependency_verification,
    )
    assert source_verification["verified"] is True
    assert dependency_verification["dependency_count"] == 73
    assert dependency_verification["translation_unit_count"] == 38
    assert dependency_verification["dependencies"] == sorted(
        conformance.EXPECTED_WP05_SOURCE_PATHS
    )

    commands = "\n".join(
        " ".join(command)
        for command in conformance.production_semantic_command_plan(
            "clang-18"
        )
    )
    for binary in (
        "meshcore_admin_dispatch_semantic",
        "meshcore_runtime_guard_semantic",
        "meshcore_trace_semantic",
        "settings_protocol_migration_semantic",
        "time_service_migration_semantic",
        "meshcore_text_plaintext_semantic",
        "meshcore_ack_completion_semantic",
        "meshcore_advert_admission_semantic",
        "meshcore_packet_hash_semantic",
        "meshcore_lifetime_semantic",
        "meshcore_rx_admission_semantic",
    ):
        assert binary in commands
    assert commands.count("-fsanitize=address,undefined") == 11
    assert not re.search(r"\bCOM\d+\b", commands, re.IGNORECASE)

    build_plans = conformance.production_semantic_command_plan("clang-18")
    dependency_plans = conformance.production_semantic_dependency_command_plan(
        "clang-18"
    )
    assert len(build_plans) == 11
    assert len(dependency_plans) == sum(
        len(spec["sources"])
        for spec in conformance.production_semantic_suite_specs()
    )
    dependency_cursor = 0
    for build, spec in zip(
        build_plans,
        conformance.production_semantic_suite_specs(),
        strict=True,
    ):
        suite_dependencies = dependency_plans[
            dependency_cursor:dependency_cursor + len(spec["sources"])
        ]
        dependency_cursor += len(spec["sources"])
        assert len(suite_dependencies) == len(spec["sources"])
        for dependency in suite_dependencies:
            assert dependency.count("-MM") == 1
            assert dependency[0:2] == ["clang-18", "-std=c11"]
            assert len([item for item in dependency if item.endswith(".c")]) == 1
            assert not {
                "-O1",
                "-g",
                "-fno-omit-frame-pointer",
                "-fsanitize=address,undefined",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pthread",
            }.intersection(dependency)
            assert {item for item in build if item.startswith(("-D", "-U"))} == {
                item for item in dependency if item.startswith(("-D", "-U"))
            }
        assert {item for item in build if item.endswith(".c")} == {
            item
            for dependency in suite_dependencies
            for item in dependency
            if item.endswith(".c")
        }


def test_remaining_wp05_fuzz_targets_are_pinned_and_fail_closed(
    tmp_path, monkeypatch
):
    manifest, seeds, source_pins = remaining_fuzz.load_inputs()

    assert [item["id"] for item in manifest["targets"]] == list(
        remaining_fuzz.TARGET_IDS
    )
    assert all(item["status"] == "implemented" for item in manifest["targets"])
    assert manifest["runs_per_target"] == 100000
    assert manifest["total_runs"] == 400000
    assert manifest["wp05_closure_ready"] is False
    assert manifest["full_semantic_matrix_covered"] is False
    assert manifest["release_closure_ready"] is False
    assert {target: len(target_seeds) for target, target_seeds in seeds.items()} == (
        remaining_fuzz.EXPECTED_SEED_COUNTS
    )
    assert source_pins["main/mesh/meshcore_crypto.c"] == canonical_lf_sha256(
        ROOT / "main/mesh/meshcore_crypto.c"
    )
    assert set(source_pins) == remaining_fuzz.EXPECTED_SOURCE_PIN_PATHS

    native_crypto = read("tests/native/meshcore_crypto_test.c")
    assert "0x44U, 0xe4U, 0x29U, 0x8fU" in native_crypto
    assert "auth before decrypt" not in native_crypto.lower()
    decrypt_fuzz = read("tests/fuzz/meshcore_decrypt_auth_fuzz.c")
    assert "tampered[0] ^= 0x01U" in decrypt_fuzz
    assert "decrypted[index] == 0x3cU" in decrypt_fuzz

    workflow = read(".github/workflows/d1l-ci.yml")
    assert "Run remaining production WP-05 fuzz targets" in workflow
    assert "python ./scripts/meshcore_remaining_fuzz_d1l.py" in workflow
    assert "--cc clang-18" in workflow
    assert "--cxx clang++-18" in workflow
    assert "--runs 100000" in workflow

    args = remaining_fuzz.parse_args(
        ["--dry-run", "--out", str(tmp_path / "remaining.json")]
    )
    status, report = remaining_fuzz.execute(args)
    assert status == 0
    assert report["status"] == "dry_run"
    assert report["source_verification"]["verified"] is True
    assert report["wp05_closure_ready"] is False
    assert report["full_semantic_matrix_covered"] is False
    assert report["release_closure_ready"] is False

    missing_crypto_pin = copy.deepcopy(manifest)
    del missing_crypto_pin["source_pins"]["main/mesh/meshcore_crypto.c"]
    missing_crypto_manifest = tmp_path / "missing_crypto_manifest.json"
    missing_crypto_manifest.write_text(
        json.dumps(missing_crypto_pin), encoding="utf-8"
    )
    monkeypatch.setattr(
        remaining_fuzz, "MANIFEST_PATH", missing_crypto_manifest
    )
    with pytest.raises(remaining_fuzz.GateFailure, match="source pin allowlist"):
        remaining_fuzz.load_inputs()


def test_signed_advert_runtime_binding_is_exact_commit_and_canonical():
    commit = "a" * 40
    receipt = completed_signed_advert_runtime_report(commit)
    binding = conformance.bind_signed_advert_runtime_receipt(receipt, commit)

    assert conformance.signed_advert_runtime_binding_valid(binding, commit)
    assert conformance.signed_advert_runtime_binding_valid(
        json.loads(json.dumps(binding, sort_keys=True)), commit
    )
    assert binding["timestamp_replay_case_count"] == 5
    assert binding["timestamp_replay_rejection_count"] == 3
    assert binding["timestamp_newer_acceptance_count"] == 2
    assert binding["replay_outcomes"] == {
        key: True for key in conformance.SIGNED_ADVERT_REPLAY_OUTCOME_KEYS
    }
    assert binding["identical_wire_hash_suppression"] == {
        "case_id": "identical_wire_hash_suppressed",
        "suppressed": True,
        "distinct_from_timestamp_replay_window": True,
    }
    conformance_report = {"signed_advert_runtime": binding}
    conformance.validate_signed_advert_runtime_binding(
        conformance_report,
        receipt,
        commit,
    )
    substituted = copy.deepcopy(conformance_report)
    substituted["signed_advert_runtime"]["canonical_sha256"] = "0" * 64
    with pytest.raises(ValueError, match="does not match"):
        conformance.validate_signed_advert_runtime_binding(
            substituted,
            receipt,
            commit,
        )
    assert binding["packet_hash_case_count"] == 5
    assert binding["packet_hash_outcomes"] == {
        key: True for key in conformance.SIGNED_ADVERT_PACKET_HASH_OUTCOME_KEYS
    }
    assert binding["simple_mesh_table_receipt"] == {
        "lookups": 10,
        "flood_duplicates": 3,
    }

    other_time = copy.deepcopy(receipt)
    other_time["generated_at"] = "2099-01-01T00:00:00+00:00"
    assert (
        conformance.bind_signed_advert_runtime_receipt(other_time, commit)[
            "canonical_sha256"
        ]
        == binding["canonical_sha256"]
    )

    stale = copy.deepcopy(receipt)
    stale["repository"]["repository_commit"] = "b" * 40
    with pytest.raises(ValueError, match="repository_commit"):
        conformance.bind_signed_advert_runtime_receipt(stale, commit)

    poisoned = copy.deepcopy(receipt)
    poisoned["result"]["wrapped_zero_timestamp_rejected"] = False
    with pytest.raises(ValueError, match="result"):
        conformance.bind_signed_advert_runtime_receipt(poisoned, commit)


def test_wp05_semantic_matrix_rejects_registry_closure_and_source_drift(tmp_path):
    oracle = conformance.load_oracle_manifest()
    matrix = json.loads(WP05_MATRIX.read_text(encoding="utf-8"))

    mutations = []
    closure_claim = copy.deepcopy(matrix)
    closure_claim["closure_ready"] = True
    mutations.append((closure_claim, "must remain fail closed"))
    missing_requirement = copy.deepcopy(matrix)
    missing_requirement["requirements"].pop()
    mutations.append((missing_requirement, "requirement registry"))
    missing_fuzz_target = copy.deepcopy(matrix)
    missing_fuzz_target["fuzz_targets"].pop()
    mutations.append((missing_fuzz_target, "fuzz-target registry"))
    zero_evidence = copy.deepcopy(matrix)
    zero_evidence["requirements"][0]["oracle_vector_groups"] = []
    zero_evidence["requirements"][0]["production_host_suites"] = []
    zero_evidence["requirements"][0]["companion_upstream_suites"] = []
    mutations.append((zero_evidence, "non-missing requirement lacks evidence"))

    for index, (payload, failure) in enumerate(mutations):
        changed = tmp_path / f"matrix-{index}.json"
        changed.write_text(json.dumps(payload), encoding="utf-8")
        with pytest.raises(conformance.GateFailure, match=failure):
            conformance.load_wp05_semantic_matrix(
                oracle, matrix_path=changed
            )

    stale_source = copy.deepcopy(matrix)
    stale_source["source_pins"]["main/mesh/store_lock.h"] = "0" * 64
    changed = tmp_path / "stale-source.json"
    changed.write_text(json.dumps(stale_source), encoding="utf-8")
    loaded = conformance.load_wp05_semantic_matrix(
        oracle, matrix_path=changed
    )
    with pytest.raises(conformance.GateFailure, match="source hash mismatch"):
        conformance.verify_wp05_semantic_sources(loaded)


def test_wp05_semantic_dependency_closure_rejects_unpinned_transitive_header():
    matrix = json.loads(WP05_MATRIX.read_text(encoding="utf-8"))
    cc = dependency_test_compiler()
    assert cc is not None
    outputs = run_dependency_outputs(cc)

    missing_pin = copy.deepcopy(matrix)
    missing_pin["source_pins"].pop("main/mesh/store_lock.h")
    source_verification = conformance.verify_wp05_semantic_sources(missing_pin)
    dependency_verification = conformance.verify_wp05_semantic_dependencies(
        missing_pin,
        outputs,
        cc,
    )
    assert dependency_verification["verified"] is False
    assert dependency_verification["unpinned_dependencies"] == [
        "main/mesh/store_lock.h"
    ]
    with pytest.raises(conformance.GateFailure, match="store_lock.h"):
        conformance.finalize_wp05_semantic_source_verification(
            source_verification,
            dependency_verification,
        )


def test_corpus_pin_is_identical_after_windows_crlf_checkout(tmp_path):
    crlf = tmp_path / "corpus.json"
    crlf.write_bytes(CORPUS.read_bytes().replace(b"\n", b"\r\n"))
    assert conformance.sha256_lf_text_file(crlf) == canonical_lf_sha256(CORPUS)

    advert_crlf = tmp_path / "advert_corpus.json"
    advert_crlf.write_bytes(ADVERT_CORPUS.read_bytes().replace(b"\n", b"\r\n"))
    assert conformance.sha256_lf_text_file(advert_crlf) == canonical_lf_sha256(
        ADVERT_CORPUS
    )


def test_upstream_source_pins_are_identical_after_windows_crlf_checkout(tmp_path):
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    for relative, expected in manifest["upstream"]["sources"].items():
        source = ROOT / "third_party" / "MeshCore" / relative
        crlf = tmp_path / Path(relative).name
        canonical = source.read_bytes().replace(b"\r\n", b"\n")
        crlf.write_bytes(canonical.replace(b"\n", b"\r\n"))
        assert conformance.sha256_lf_text_file(crlf) == expected


def test_harness_has_fixed_bidirectional_matrix_and_structural_sweeps():
    harness = read("tests/meshcore_conformance/meshcore_wire_conformance.cpp")
    fuzzer = read("tests/meshcore_conformance/meshcore_wire_fuzz.cpp")
    advert_fuzzer = read("tests/meshcore_conformance/meshcore_advert_fuzz.cpp")
    shim = read("tests/meshcore_conformance/meshcore_packet_checked.hpp")
    sha_stub = read("tests/meshcore_conformance/stubs/SHA256.h")

    assert "kExpectedPerDirection == 504" in harness
    assert "upstream_to_local == 504U" in harness
    assert "local_to_upstream == 504U" in harness
    assert "value <= 0xFFU" in harness
    assert "result.valid != 119U" in harness
    assert "result.invalid != 137U" in harness
    assert "kRequired == 254U" in harness
    assert "data_region_unchanged" in harness
    assert "verify_payload_version_gate" in harness
    assert "payload_version_gate" in harness
    assert "d1l_meshcore_wire_decode" in fuzzer
    assert "d1l_meshcore_wire_decode_v1" in fuzzer
    assert "d1l_meshcore_wire_encode" in fuzzer
    assert "packet.header != data[0]" in fuzzer
    assert "packet.version != ((data[0] >> 6U) & 0x03U)" in fuzzer
    assert "D1L_MESHCORE_MAX_PACKET_PAYLOAD == MAX_PACKET_PAYLOAD" in shim
    assert "D1L_MESHCORE_ROUTE_DIRECT == ROUTE_TYPE_DIRECT" in shim
    assert "D1L_MESHCORE_PAYLOAD_MULTIPART == PAYLOAD_TYPE_MULTIPART" in shim
    assert "D1L_MESHCORE_PAYLOAD_VER_1 == PAYLOAD_VER_1" in shim
    assert "Packet" not in fuzzer
    assert "d1l_advert_data_parse" in advert_fuzzer
    assert "local_advert_parser" not in advert_fuzzer
    assert "canonical_zeroed_with_type_N" not in advert_fuzzer
    assert "std::memcmp(&first, &second, sizeof(first)) == 0" in advert_fuzzer
    assert "size <= D1L_ADVERT_DATA_MAX_LEN" in advert_fuzzer
    assert "first.lat_e6 >= -90000000" in advert_fuzzer
    assert "first.lon_e6 >= -180000000" in advert_fuzzer
    assert "MAX_PACKET_PAYLOAD == 184" in shim
    assert "std::abort()" in sha_stub
    assert "update(const void *" in sha_stub


def test_v1_decoder_rejects_future_versions_without_changing_output():
    source = read("main/mesh/meshcore_wire.c")
    header = read("main/mesh/meshcore_wire.h")
    runner = read("scripts/meshcore_conformance_d1l.py")
    boundary = read("docs/MESHCORE_CONFORMANCE.md")

    assert "#define D1L_MESHCORE_PAYLOAD_VER_1 0x00U" in header
    assert "bool d1l_meshcore_wire_decode_v1" in header
    body = source.split("bool d1l_meshcore_wire_decode_v1", 1)[1].split(
        "bool d1l_meshcore_wire_write_prefix", 1
    )[0]
    assert "d1l_meshcore_wire_packet_t packet;" in body
    assert "d1l_meshcore_wire_decode(raw, size, &packet)" in body
    assert "packet.version != D1L_MESHCORE_PAYLOAD_VER_1" in body
    assert body.index("packet.version !=") < body.index("*out_packet = packet")
    assert 'vector_result.get("payload_version_gate", {}).get("tested") != 4' in runner
    assert "d1l_meshcore_wire_decode_v1()" in boundary
    assert "leaves its output unchanged" in boundary


def test_production_service_uses_the_codec_exercised_by_the_harness():
    service = read("main/mesh/meshcore_service.c")
    admin_runtime = read("main/mesh/meshcore_admin_runtime.c")
    cmake = read("main/CMakeLists.txt")

    assert '#include "mesh/meshcore_wire.h"' in service
    assert '"mesh/meshcore_wire.c"' in cmake
    # Channel, DM, ACK, PATH, TRACE hash binding, advert, and authenticated
    # admin RESPONSE each enter through the same fail-closed decoder.
    assert service.count("d1l_meshcore_wire_decode_v1(") == 8
    assert service.count("d1l_meshcore_wire_decode(") == 0
    ack_builder = service.split("static esp_err_t build_dm_ack_response", 1)[1].split(
        "static bool dispatch_bounded_dm_ack", 1
    )[0]
    # Existing packet builders plus admin ANON_REQ, status REQ, and the exact
    # allowlisted mutation REQ use the same production prefix encoder.
    assert service.count("d1l_meshcore_wire_write_prefix(") == 7
    assert admin_runtime.count("d1l_meshcore_wire_write_prefix(") == 3
    assert ack_builder.count("d1l_meshcore_wire_write_prefix(") == 3
    assert (
        service.count("d1l_meshcore_wire_write_prefix(")
        + admin_runtime.count("d1l_meshcore_wire_write_prefix(")
        - ack_builder.count("d1l_meshcore_wire_write_prefix(")
        == 7
    )
    assert "parse_wire_packet" not in service
    for legacy_helper in [
        "path_len_valid",
        "path_hash_size",
        "path_hash_count",
        "path_byte_len",
    ]:
        assert not re.search(
            rf"static\s+(?:bool|uint8_t)\s+{legacy_helper}\s*\(", service
        )


def test_documented_ci_cli_dry_run_is_fail_closed(tmp_path):
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    output = tmp_path / "meshcore_conformance.json"
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--cc",
            "clang-18",
            "--cxx",
            "clang++-18",
            "--commit",
            commit,
            "--seed",
            "13746277",
            "--runs",
            "100000",
            "--out",
            str(output),
            "--dry-run",
        ],
        cwd=ROOT,
        env=dry_run_environment(),
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
    report = json.loads(output.read_text(encoding="utf-8"))
    assert report["passed"] is False
    assert report["execution_complete"] is False
    assert report["status"] == "dry_run"
    assert report["coverage_boundary"] == "wire_envelope_only"
    assert report["coverage_level"] == "wire_envelope_only"
    assert report["semantic_coverage_level"] == "partial_declared_host_semantics"
    assert report["issue_65_closure_eligible"] is False
    assert report["closure_ready"] is False
    assert report["source_verification"]["verified"] is True
    assert report["source_verification"]["gitlink_mode"] == "160000"
    assert report["source_verification"]["gitlink_commit"] == (
        "e8d3c53ba1ea863937081cd0caad759b832f3028"
    )
    assert report["source_verification"]["checkout_commit"] == commit
    assert report["source_verification"]["allowlisted_source_sha256_verified"] is True
    assert report["source_verification"]["source_hash_mode"] == (
        "canonical_lf_text_sha256"
    )
    assert report["corpus"]["verified"] is True
    assert report["advert_corpus"]["verified"] is True
    assert report["advert_corpus"]["coverage_boundary"] == "local_advert_parser"
    assert report["advert_corpus"]["seed_count"] == 8
    assert report["scope"]["fuzz_target"] == "local_wire_decoder_only"
    assert report["scope"]["semantic_fuzz_targets"] == ["local_advert_parser"]
    assert report["scope"]["advert_parser_fuzz_covered"] is True
    assert report["scope"]["packet_semantics_covered"] is False
    assert report["scope"]["bounded_production_semantics_covered"] is True
    assert report["wp05_semantic_matrix"]["closure_ready"] is False
    assert report["wp05_semantic_matrix"]["production_scenario_count"] == 75
    assert report["signed_advert_runtime"] is None
    semantic_source = report["wp05_semantic_matrix"]["source_verification"]
    assert semantic_source["pins_verified"] is True
    assert semantic_source["dependency_closure_verified"] is False
    assert semantic_source["dependency_verification"] is None
    assert semantic_source["verified"] is False
    assert report["wp05_semantic_matrix"]["result"] is None
    assert len(report["vector_matrix"]["payload_types"]) == 7
    assert report["payload_version_gate"] == {
        "permissive_envelope_versions": [0, 1, 2, 3],
        "accepted_v1_versions": [0],
        "rejected_future_versions": [1, 2, 3],
        "preserve_output_on_reject": True,
    }
    assert report["zero_overrun"] is None
    assert report["zero_corruption"] is None
    commands = "\n".join(" ".join(command) for command in report["commands"])
    assert "third_party\\MeshCore\\src\\Packet.cpp" in commands or (
        "third_party/MeshCore/src/Packet.cpp" in commands
    )
    assert "-fsanitize=fuzzer,address,undefined" in commands
    assert "-fsanitize=address,undefined" in commands
    assert "main\\mesh\\advert_data.c" in commands or "main/mesh/advert_data.c" in commands
    assert "meshcore_advert_fuzz" in commands
    assert not re.search(r"\bCOM\d+\b", commands, re.IGNORECASE)


def test_manifest_validation_rejects_empty_upstream_source_allowlist(tmp_path, monkeypatch):
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    manifest["upstream"]["sources"] = {}
    changed = tmp_path / "manifest.json"
    changed.write_text(json.dumps(manifest), encoding="utf-8")
    monkeypatch.setattr(conformance, "MANIFEST_PATH", changed)

    with pytest.raises(conformance.GateFailure, match="source allowlist"):
        conformance.load_manifest()


def test_github_actions_cannot_turn_dry_run_into_a_green_gate(tmp_path):
    output = tmp_path / "forbidden-dry-run.json"
    env = os.environ.copy()
    env["D1L_MESHCORE_CONFORMANCE_CI"] = "1"
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--out",
            str(output),
            "--dry-run",
        ],
        cwd=ROOT,
        env=env,
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode != 0
    report = json.loads(output.read_text(encoding="utf-8"))
    assert report["status"] == "fail"
    assert report["passed"] is False
    assert "dry-run is forbidden in GitHub Actions" in report["failure"]


def test_completed_report_validator_rejects_semantically_incomplete_green_receipts(
    tmp_path, monkeypatch
):
    commit = "a" * 40
    report = completed_report(commit, ROOT / ".github" / "d1l-build-inputs.json")
    signed_receipt = completed_signed_advert_runtime_report(commit)
    with pytest.raises(ValueError, match="signed_advert_runtime_receipt_binding"):
        conformance.validate_completed_report(report, commit)
    conformance.validate_completed_report(
        report,
        commit,
        signed_advert_runtime_receipt=signed_receipt,
    )
    canonical = conformance.canonicalize_release_report(report)
    conformance.validate_completed_report(
        canonical,
        commit,
        require_generated_at=False,
        signed_advert_runtime_receipt=signed_receipt,
    )

    mutations = []
    incomplete_fuzz = copy.deepcopy(report)
    incomplete_fuzz["fuzz_result"]["completed_runs"] = 99999
    mutations.append((incomplete_fuzz, "fuzz_complete"))
    incomplete_advert_fuzz = copy.deepcopy(report)
    incomplete_advert_fuzz["advert_fuzz_result"]["completed_runs"] = 99999
    mutations.append((incomplete_advert_fuzz, "advert_fuzz_complete"))
    stale_advert_corpus = copy.deepcopy(report)
    stale_advert_corpus["advert_corpus"]["manifest_sha256"] = "0" * 64
    mutations.append((stale_advert_corpus, "advert_corpus"))
    incomplete_vectors = copy.deepcopy(report)
    incomplete_vectors["vector_result"]["vectors"]["total"] = 863
    mutations.append((incomplete_vectors, "vector_counts"))
    incomplete_semantic_matrix = copy.deepcopy(report)
    incomplete_semantic_matrix["wp05_semantic_matrix"]["result"][
        "scenario_count"
    ] = 33
    mutations.append((incomplete_semantic_matrix, "wp05_semantic_matrix"))
    fabricated_semantic_scenarios = copy.deepcopy(report)
    fabricated_semantic_scenarios["wp05_semantic_matrix"]["result"]["suites"][
        0
    ]["scenario_ids"] = [f"fabricated-{index}" for index in range(4)]
    mutations.append((fabricated_semantic_scenarios, "wp05_semantic_matrix"))
    tampered_semantic_summary = copy.deepcopy(report)
    tampered_requirement = tampered_semantic_summary["wp05_semantic_matrix"][
        "requirements"
    ][0]
    tampered_requirement["oracle_vector_groups"] = []
    tampered_requirement["production_host_suites"] = []
    tampered_requirement["oracle_semantic_vector_evidence"] = {}
    tampered_requirement["referenced_semantic_vectors"] = 0
    mutations.append((tampered_semantic_summary, "wp05_semantic_matrix"))
    incomplete_semantic_dependencies = copy.deepcopy(report)
    incomplete_semantic_dependencies["wp05_semantic_matrix"][
        "source_verification"
    ]["dependency_verification"]["dependency_count"] = 45
    mutations.append(
        (incomplete_semantic_dependencies, "wp05_semantic_matrix")
    )
    substituted_semantic_compiler = copy.deepcopy(report)
    for command in substituted_semantic_compiler["wp05_semantic_matrix"][
        "source_verification"
    ]["dependency_verification"]["commands"]:
        command[0] = "attacker-cc"
    mutations.append(
        (substituted_semantic_compiler, "wp05_semantic_matrix")
    )
    tampered_signed_runtime = copy.deepcopy(report)
    tampered_signed_runtime["signed_advert_runtime"]["replay_outcomes"][
        "distinct_equal_timestamp_rejected"
    ] = False
    mutations.append((tampered_signed_runtime, "signed_advert_runtime"))
    stale_signed_runtime_commit = copy.deepcopy(report)
    stale_signed_runtime_commit["signed_advert_runtime"]["repository_commit"] = (
        "b" * 40
    )
    mutations.append((stale_signed_runtime_commit, "signed_advert_runtime"))
    substituted_signed_runtime_digest = copy.deepcopy(report)
    substituted_signed_runtime_digest["signed_advert_runtime"][
        "canonical_sha256"
    ] = "0" * 64
    mutations.append(
        (
            substituted_signed_runtime_digest,
            "signed_advert_runtime_receipt_binding",
        )
    )
    stale_source = copy.deepcopy(report)
    stale_source["source_verification"]["source_files"]["src/Packet.cpp"][
        "matched"
    ] = False
    mutations.append((stale_source, "source_hashes"))
    unverified_compiler = copy.deepcopy(report)
    unverified_compiler["toolchain"]["clang"]["bytes_verified"] = False
    mutations.append((unverified_compiler, "toolchain_bytes"))
    hidden_sanitizer_exception = copy.deepcopy(report)
    hidden_sanitizer_exception["sanitizer_policy"]["exceptions"] = [
        {"source": "hidden.c", "compiler_flag": "-fno-sanitize=undefined"}
    ]
    mutations.append((hidden_sanitizer_exception, "sanitizer_policy"))
    false_full_ubsan_claim = copy.deepcopy(report)
    false_full_ubsan_claim["full_ubsan_clean"] = False
    mutations.append((false_full_ubsan_claim, "full_ubsan_clean_true"))
    unverified_sanitizer_policy = copy.deepcopy(report)
    unverified_sanitizer_policy["sanitizer_policy_passed"] = False
    mutations.append((unverified_sanitizer_policy, "sanitizer_policy_passed"))
    sanitizer_suppression = copy.deepcopy(report)
    sanitizer_suppression["commands"][5].insert(1, "-fno-sanitize=shift-base")
    mutations.append(
        (sanitizer_suppression, "sanitizer_disable_flags_absent")
    )
    unrelated_sanitizer_suppression = copy.deepcopy(report)
    unrelated_sanitizer_suppression["commands"][5].insert(
        1,
        "-fno-sanitize=undefined",
    )
    mutations.append(
        (unrelated_sanitizer_suppression, "sanitizer_disable_flags_absent")
    )
    string_form_sanitizer_suppression = copy.deepcopy(report)
    string_form_sanitizer_suppression["commands"][0] = (
        "clang-18 -fno-sanitize=undefined source.c"
    )
    mutations.append((string_form_sanitizer_suppression, "commands"))
    weakened_non_exception_sanitizers = copy.deepcopy(report)
    weakened_non_exception_sanitizers["commands"][0] = [
        (
            "-fsanitize=address"
            if argument == "-fsanitize=address,undefined"
            else argument
        )
        for argument in weakened_non_exception_sanitizers["commands"][0]
    ]
    mutations.append((weakened_non_exception_sanitizers, "commands"))

    for payload, failure in mutations:
        with pytest.raises(ValueError, match=failure):
            conformance.validate_completed_report(
                payload,
                commit,
                signed_advert_runtime_receipt=signed_receipt,
            )
        canonical_payload = conformance.canonicalize_release_report(payload)
        with pytest.raises(ValueError, match=failure):
            conformance.validate_completed_report(
                canonical_payload,
                commit,
                require_generated_at=False,
                signed_advert_runtime_receipt=signed_receipt,
            )

    poisoned_matrix = json.loads(WP05_MATRIX.read_text(encoding="utf-8"))
    poisoned_matrix["source_pins"]["main/mesh/store_lock.h"] = "0" * 64
    poisoned_matrix_path = tmp_path / "poisoned-wp05-semantic-matrix.json"
    poisoned_matrix_path.write_text(
        json.dumps(poisoned_matrix, indent=2) + "\n",
        encoding="utf-8",
    )
    poisoned_receipt = copy.deepcopy(report)
    poisoned_receipt["wp05_semantic_matrix"]["sha256"] = canonical_lf_sha256(
        poisoned_matrix_path
    )
    monkeypatch.setattr(
        conformance,
        "WP05_SEMANTIC_MATRIX_PATH",
        poisoned_matrix_path,
    )
    with pytest.raises(ValueError, match="wp05_semantic_matrix"):
        conformance.validate_completed_report(
            poisoned_receipt,
            commit,
            signed_advert_runtime_receipt=signed_receipt,
        )
    poisoned_canonical = conformance.canonicalize_release_report(poisoned_receipt)
    with pytest.raises(ValueError, match="wp05_semantic_matrix"):
        conformance.validate_completed_report(
            poisoned_canonical,
            commit,
            require_generated_at=False,
            signed_advert_runtime_receipt=signed_receipt,
        )


def test_runner_persists_fuzzer_reproducers_and_records_elapsed_time():
    runner = SCRIPT.read_text(encoding="utf-8")
    assert "meshcore_conformance_findings_" in runner
    assert "meshcore_advert_fuzz_findings_" in runner
    assert 'f"-artifact_prefix={findings_dir}{os.sep}"' in runner
    assert 'f"-artifact_prefix={advert_findings_dir}{os.sep}"' in runner
    assert '"duration_ms": fuzz_duration_ms' in runner
    assert '"duration_ms": advert_fuzz_duration_ms' in runner
    assert 'report["zero_overrun"] = True' in runner
    assert 'report["zero_corruption"] = True' in runner


def test_wrong_seed_writes_failure_json_and_exits_nonzero(tmp_path):
    output = tmp_path / "failed.json"
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--seed",
            "1",
            "--runs",
            "100000",
            "--out",
            str(output),
            "--dry-run",
        ],
        cwd=ROOT,
        env=dry_run_environment(),
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode != 0
    report = json.loads(output.read_text(encoding="utf-8"))
    assert report["passed"] is False
    assert report["status"] == "fail"
    assert "deterministic seed 13746277" in report["failure"]
