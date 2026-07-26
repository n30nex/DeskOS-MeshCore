import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "tests" / "meshcore_packet_semantics" / "corpus.json"
MANIFEST = ROOT / "tests" / "meshcore_packet_semantics" / "manifest.json"
MARKER = "native MeshCore semantic packet parser: ok (100000 deterministic cases)"


def canonical_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def compiler() -> str:
    selected = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
    if not selected:
        raise AssertionError("a C compiler is required for semantic packet tests")
    return selected


def compile_command(output: Path, test_source: Path) -> list[str]:
    return [
        compiler(),
        "-std=c11",
        "-O1",
        "-g",
        "-fno-omit-frame-pointer",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main" / "mesh" / "meshcore_wire.c"),
        str(ROOT / "main" / "mesh" / "meshcore_packet_semantics.c"),
        str(test_source),
        "-o",
        str(output),
    ]


def test_native_semantic_parser_executes_100000_cases(tmp_path: Path):
    executable = tmp_path / (
        "meshcore_packet_semantics_test.exe"
        if os.name == "nt"
        else "meshcore_packet_semantics_test"
    )
    subprocess.run(
        compile_command(
            executable, ROOT / "tests" / "native" / "meshcore_packet_semantics_test.c"
        ),
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    completed = subprocess.run(
        [str(executable)], cwd=ROOT, check=True, capture_output=True, text=True
    )
    assert completed.stdout.strip() == MARKER


def test_production_receive_dispatch_is_semantic_preflight_bound():
    source = (ROOT / "main" / "mesh" / "meshcore_service.c").read_text(
        encoding="utf-8"
    )
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    start = source.index("static void meshcore_service_handle_radio_rx_done(")
    end = source.index("static void meshcore_service_handle_radio_rx_timeout", start)
    dispatch = source[start:end]

    assert '"mesh/meshcore_packet_semantics.c"' in cmake
    assert '#include "mesh/meshcore_packet_semantics.h"' in source
    assert dispatch.count("d1l_meshcore_packet_semantic_parse(") == 1
    assert "switch (packet.kind)" in dispatch
    for kind in (
        "ADMIN_RESPONSE",
        "CHANNEL_TEXT",
        "DIRECT_MESSAGE",
        "ACK",
        "MULTIPART_ACK",
        "PATH",
        "TRACE",
        "ADVERT",
        "CONTROL",
    ):
        assert f"D1L_MESHCORE_PACKET_SEMANTIC_{kind}" in dispatch


def test_corpus_and_manifest_are_hash_bound():
    corpus = json.loads(CORPUS.read_text(encoding="utf-8"))
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    assert corpus["encoding"] == "hex"
    assert corpus["coverage_boundary"] == (
        "production_meshcore_semantic_packet_admission"
    )
    assert len(corpus["seeds"]) == 14
    assert manifest["corpus"] == {
        "path": "tests/meshcore_packet_semantics/corpus.json",
        "seed_count": 14,
        "sha256": canonical_sha256(CORPUS),
    }
    assert manifest["engine"] == "libFuzzer"
    assert manifest["runs"] == 100000
    assert manifest["seed"] == 13746277
    assert manifest["max_input_bytes"] == 256
    assert manifest["sanitizers"] == ["address", "undefined"]
    assert manifest["source_pins"] == {
        relative: canonical_sha256(ROOT / relative)
        for relative in manifest["source_pins"]
    }
    assert manifest["side_effect_free_rejection"] is True
    assert manifest["decrypt_auth_covered"] is False
    assert manifest["wp05_closure_ready"] is False
    assert manifest["release_closure_ready"] is False
    for seed in corpus["seeds"]:
        payload = bytes.fromhex(seed["hex"])
        assert payload
        assert hashlib.sha256(payload).hexdigest() == seed["sha256"]


def test_libfuzzer_target_is_executable_when_clang_is_available(tmp_path: Path):
    if os.name == "nt":
        pytest.skip("libFuzzer execution is covered by the pinned Linux Actions job")
    clang = shutil.which("clang-18") or shutil.which("clang")
    if not clang:
        pytest.skip("libFuzzer execution is covered by the pinned Clang Actions job")

    executable = tmp_path / "meshcore_packet_semantics_fuzz"
    command = compile_command(
        executable, ROOT / "tests" / "fuzz" / "meshcore_packet_semantics_fuzz.c"
    )
    command[0] = clang
    command.insert(command.index("-Wall"), "-fsanitize=fuzzer,address,undefined")
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)

    corpus_data = json.loads(CORPUS.read_text(encoding="utf-8"))
    corpus_dir = tmp_path / "corpus"
    corpus_dir.mkdir()
    for index, seed in enumerate(corpus_data["seeds"]):
        (corpus_dir / f"{index:02d}_{seed['name']}").write_bytes(
            bytes.fromhex(seed["hex"])
        )
    findings = tmp_path / "findings"
    findings.mkdir()
    completed = subprocess.run(
        [
            str(executable),
            "-runs=100000",
            "-seed=13746277",
            "-max_len=256",
            "-print_final_stats=1",
            f"-artifact_prefix={findings}{os.sep}",
            str(corpus_dir),
        ],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
        timeout=120,
        env={
            **os.environ,
            "ASAN_OPTIONS": (
                "abort_on_error=1:detect_leaks=1:halt_on_error=1:"
                "strict_string_checks=1"
            ),
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        },
    )
    assert re.search(r"#100000\s+DONE\b", completed.stderr)
    assert list(findings.iterdir()) == []


def test_execution_is_pinned_to_linux_clang18_actions_job():
    workflow = (ROOT / ".github" / "workflows" / "d1l-ci.yml").read_text(
        encoding="utf-8"
    )
    conformance_job = workflow.split("\n  meshcore-conformance:", 1)[1].split(
        "\n  rp2040-sd-bridge-build:", 1
    )[0]
    assert "runs-on: ubuntu-24.04" in conformance_job
    assert "Install pinned Clang sanitizer toolchain" in conformance_job
    assert "Run production MeshCore semantic packet parser fuzz gate" in conformance_job
    assert "python ./scripts/meshcore_semantic_packet_fuzz_d1l.py" in conformance_job
    assert "--cc clang-18" in conformance_job
    assert "--seed 13746277" in conformance_job
    assert "--runs 100000" in conformance_job


def test_fuzz_receipt_dry_run_is_exact_source_bound(tmp_path: Path):
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, check=True,
        capture_output=True, text=True,
    ).stdout.strip()
    receipt = tmp_path / "meshcore-semantic-packet-dry-run.json"
    subprocess.run(
        [
            sys.executable,
            os.fspath(ROOT / "scripts" / "meshcore_semantic_packet_fuzz_d1l.py"),
            "--commit",
            commit,
            "--dry-run",
            "--out",
            os.fspath(receipt),
        ],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    report = json.loads(receipt.read_text(encoding="utf-8"))
    assert report["artifact_type"] == "d1l_meshcore_semantic_packet_fuzz"
    assert report["status"] == "dry_run"
    assert report["passed"] is False
    assert report["execution_complete"] is False
    assert report["repository_commit"] == commit
    assert report["source_verification"]["verified"] is True
    assert report["corpus"]["seed_count"] == 14
    assert report["wp05_closure_ready"] is False
    assert report["release_closure_ready"] is False
