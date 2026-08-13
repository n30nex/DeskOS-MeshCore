from __future__ import annotations

import importlib.util
import io
import json
import tarfile
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = ROOT / "scripts" / "meshcore_signed_advert_runtime_d1l.py"
MANIFEST_PATH = (
    ROOT / "tests" / "meshcore_signed_advert_runtime" / "manifest.json"
)


def load_module():
    spec = importlib.util.spec_from_file_location(
        "meshcore_signed_advert_runtime_d1l", SCRIPT_PATH
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_signed_advert_runtime_manifest_is_bounded_and_pinned():
    module = load_module()
    manifest = module.load_manifest()

    assert manifest["schema_version"] == 1
    assert manifest["work_package"] == "WP-04"
    assert manifest["capability"] == "identity_signed_advert_semantic_runtime"
    assert manifest["wp04_closure_eligible"] is False
    assert manifest["closure_ready"] is False
    assert manifest["source_hash_mode"] == "canonical_lf_text_sha256"
    assert manifest["upstream"]["commit"] == (
        "e8d3c53ba1ea863937081cd0caad759b832f3028"
    )
    dependency = manifest["external_dependency"]
    assert dependency["name"] == "rweather/Crypto"
    assert dependency["version"] == "0.4.0"
    assert dependency["archive_size"] == 162696
    assert dependency["archive_sha256"] == (
        "1867740aad0d61bdcbac25f6dbc8eefe6eed9e7b37f48d9d0b9d80500ad431e0"
    )
    assert dependency["release_commit"] == (
        "61e84b220fc8dfe83c2e04716d0fa88cfaddadf6"
    )
    assert manifest["sanitizer_policy"]["full_ubsan_clean"] is True
    assert manifest["sanitizer_policy"]["exceptions"] == []
    assert manifest["sanitizer_policy"]["source_level_remediation"]["status"] == (
        "resolved"
    )
    assert "firmware_runtime_binding_and_real_peer_interoperability" in manifest[
        "residual_gaps"
    ]
    assert "login_request_response_admin" in manifest["residual_gaps"]
    assert (
        "five_distinct_validly_signed_timestamp_cases_bypass_hash_suppression"
        in manifest["assertions"]
    )
    assert manifest["expected_result"]["signed_adverts_sent"] == 6
    assert manifest["expected_result"]["timestamp_replay_cases"] == 5
    assert manifest["expected_result"]["timestamp_replay_rejections"] == 3
    assert manifest["expected_result"]["timestamp_newer_acceptances"] == 2
    assert manifest["expected_result"]["filter_calls"] == 10
    assert manifest["expected_result"]["table_lookups"] == 10
    assert manifest["expected_result"]["table_flood_duplicates"] == 3
    assert manifest["expected_result"]["packet_hash_parity_cases"] == 5
    assert manifest["expected_result"]["real_simple_mesh_tables"] is True
    assert manifest["expected_result"]["upstream_d1l_packet_hash_match"] is True
    assert (
        manifest["expected_result"]["route_path_transport_variants_suppressed"]
        is True
    )
    assert manifest["expected_result"]["allocations"] == 17
    assert manifest["expected_result"]["releases"] == 17


def test_signed_advert_runtime_repository_sources_match_exact_pins():
    module = load_module()
    manifest = module.load_manifest()
    receipt = module.verify_repository_sources(manifest)

    assert receipt["verified"] is True
    assert receipt["upstream_commit"] == manifest["upstream"]["commit"]
    assert receipt["gitlink_commit"] == manifest["upstream"]["commit"]
    assert receipt["source_hash_mode"] == "canonical_lf_text_sha256"
    assert len(receipt["files"]) == 45
    assert all(item["matched"] for item in receipt["files"].values())


def test_repository_source_hashing_normalizes_windows_crlf(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    module = load_module()
    source = tmp_path / "sample.cpp"
    source.write_bytes(b"line one\nline two\n")
    expected = module.sha256_lf_text_file(source)
    source.write_bytes(b"line one\r\nline two\r\n")
    assert module.sha256_file(source) != expected

    commit = "1" * 40
    repository_commit = "2" * 40
    manifest = {
        "source_hash_mode": "canonical_lf_text_sha256",
        "upstream": {
            "path": "third_party/MeshCore",
            "commit": commit,
            "sources": {"sample.cpp": expected},
        },
        "vendored_crypto_sources": {},
        "host_sources": {},
    }

    def fake_git_output(*args: str) -> str:
        if args[0] == "-C":
            return commit
        if args[0] == "ls-tree":
            return f"160000 commit {commit}\tthird_party/MeshCore"
        assert args == ("rev-parse", "HEAD")
        return repository_commit

    monkeypatch.setattr(module, "ROOT", tmp_path)
    monkeypatch.setattr(module, "git_output", fake_git_output)
    receipt = module.verify_repository_sources(manifest, repository_commit)

    assert receipt["verified"] is True
    assert receipt["files"]["sample.cpp"]["actual_sha256"] == expected
    assert receipt["files"]["sample.cpp"]["matched"] is True


def test_signed_advert_runtime_command_plan_compiles_real_sources():
    module = load_module()
    commands = module.command_plan(
        "clang-18",
        "clang++-18",
        "/tmp/crypto",
        "/tmp/build",
        sanitize=True,
    )
    flattened = [" ".join(command).replace("\\", "/") for command in commands]

    for source in (
        "third_party/MeshCore/src/Identity.cpp",
        "third_party/MeshCore/src/Packet.cpp",
        "third_party/MeshCore/src/Dispatcher.cpp",
        "third_party/MeshCore/src/Mesh.cpp",
        "third_party/MeshCore/src/Utils.cpp",
        "third_party/MeshCore/src/helpers/AdvertDataHelpers.cpp",
        "third_party/MeshCore/src/helpers/BaseChatMesh.cpp",
        "main/mesh/meshcore_packet_hash.c",
        "tests/meshcore_signed_advert_runtime/mbedtls_md_sha256_bridge.cpp",
    ):
        assert sum(source in command for command in flattened) == 1
    for source in (
        "/tmp/crypto/Ed25519.cpp",
        "/tmp/crypto/Curve25519.cpp",
        "/tmp/crypto/BigNumberUtil.cpp",
        "/tmp/crypto/SHA512.cpp",
        "/tmp/crypto/Hash.cpp",
        "/tmp/crypto/Crypto.cpp",
    ):
        assert sum(source in command for command in flattened) == 1
    assert not any("-fno-sanitize=" in command for command in flattened)
    for source in (
        "overlays/meshcore_ed25519_defined/fe.c",
        "overlays/meshcore_ed25519_defined/ge.c",
        "overlays/meshcore_ed25519_defined/sc.c",
    ):
        assert sum(source in command for command in flattened) == 1
    for source in (
        "third_party/MeshCore/lib/ed25519/fe.c",
        "third_party/MeshCore/lib/ed25519/ge.c",
        "third_party/MeshCore/lib/ed25519/sc.c",
    ):
        assert not any(source in command for command in flattened)
    assert not any("meshcore_oracle.cpp" in command for command in flattened)
    assert flattened[-1] in {
        "/tmp/build/meshcore_signed_advert_runtime",
        "/tmp/build/meshcore_signed_advert_runtime.exe",
    }


def test_external_source_extraction_is_hash_locked_and_path_safe(tmp_path: Path):
    module = load_module()
    content = b"exact external source\n"
    archive_buffer = io.BytesIO()
    with tarfile.open(fileobj=archive_buffer, mode="w:gz") as archive:
        member = tarfile.TarInfo("Ed25519.h")
        member.size = len(content)
        archive.addfile(member, io.BytesIO(content))
    dependency = {
        "sources": {"Ed25519.h": module.sha256_bytes(content)},
    }

    receipt = module.extract_external_sources(
        archive_buffer.getvalue(), dependency, tmp_path / "safe"
    )
    assert receipt["verified"] is True
    assert (tmp_path / "safe" / "Ed25519.h").read_bytes() == content

    unsafe_buffer = io.BytesIO()
    with tarfile.open(fileobj=unsafe_buffer, mode="w:gz") as archive:
        member = tarfile.TarInfo("../Ed25519.h")
        member.size = len(content)
        archive.addfile(member, io.BytesIO(content))
    with pytest.raises(module.GateFailure, match="unsafe path"):
        module.extract_external_sources(
            unsafe_buffer.getvalue(), dependency, tmp_path / "unsafe"
        )


def test_signed_advert_runtime_is_an_actions_gate():
    workflow = (ROOT / ".github" / "workflows" / "d1l-ci.yml").read_text(
        encoding="utf-8"
    )
    assert "Run pinned MeshCore signed-advert semantic runtime" in workflow
    assert "scripts/meshcore_signed_advert_runtime_d1l.py" in workflow
    assert "--sanitize" in workflow
    assert "meshcore_signed_advert_runtime_${D1L_SOURCE_SHA}.json" in workflow
    assert workflow.index(
        "Run pinned MeshCore signed-advert semantic runtime"
    ) < workflow.index("Run MeshCore wire-envelope conformance and RX fuzzing")
    assert (
        '--signed-advert-runtime-receipt "artifacts/meshcore-conformance/'
        'meshcore_signed_advert_runtime_${D1L_SOURCE_SHA}.json"'
    ) in workflow
    assert "signed_advert_runtime_receipt=signed_runtime" in workflow


def test_runtime_harness_uses_production_dispatch_not_projection_helpers():
    harness = (
        ROOT
        / "tests"
        / "meshcore_signed_advert_runtime"
        / "meshcore_signed_advert_runtime.cpp"
    ).read_text(encoding="utf-8")
    assert "public BaseChatMesh" in harness
    assert "createSelfAdvert" in harness
    assert "sendFlood" in harness
    assert "sender.loop()" in harness
    assert "receiver.loop()" in harness
    assert "duplicate advert reached callback" in harness
    assert "SimpleMeshTables::hasSeen(packet)" in harness
    assert "d1l_meshcore_packet_hash_calculate" in harness
    assert "routed duplicate advert reached callback" in harness
    assert "distinct equal-timestamp advert reached callback" in harness
    assert "distinct older-timestamp advert reached callback" in harness
    assert "strictly newer timestamp did not reach callback" in harness
    assert "UINT32_MAX timestamp did not reach callback" in harness
    assert "wrapped zero timestamp reached callback" in harness
    assert "timestamp scenario reused an earlier wire hash input" in harness
    assert 'transmit_distinct(baseline_timestamp, "equal-replay")' in harness
    assert 'transmit_distinct(UINT32_MAX, "max-node")' in harness
    assert 'transmit_distinct(0U, "wrapped-zero")' in harness
    assert "forged advert reached callback" in harness
    assert "self advert reached callback" in harness
    assert "packet ownership is imbalanced" in harness
    assert "d1l_meshcore_oracle_" not in harness


def test_manifest_json_has_no_duplicate_keys():
    def reject_duplicates(pairs):
        result = {}
        for key, value in pairs:
            assert key not in result, f"duplicate JSON key: {key}"
            result[key] = value
        return result

    json.loads(MANIFEST_PATH.read_text(encoding="utf-8"), object_pairs_hook=reject_duplicates)
