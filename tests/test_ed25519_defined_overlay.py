import json
import shutil
import subprocess
import sys
from pathlib import Path

from scripts import validate_ed25519_defined_overlay as validator
from scripts import meshcore_conformance_d1l as conformance_gate
from scripts import meshcore_signed_advert_runtime_d1l as signed_runtime_gate


ROOT = Path(__file__).resolve().parents[1]


def test_checked_in_overlay_is_exact_mechanical_transform_with_preserved_license():
    receipt = validator.validate_checked_in_overlay()

    assert {
        name: entry["transformed_expressions"] for name, entry in receipt.items()
    } == validator.EXPECTED_TRANSFORM_COUNTS
    assert sum(validator.EXPECTED_TRANSFORM_COUNTS.values()) == 215


def test_overlay_has_no_declared_sanitizer_exception_flags():
    source = (ROOT / "scripts" / "validate_ed25519_defined_overlay.py").read_text(
        encoding="utf-8"
    )
    required_build = source[source.index("if sanitizer:") : source.index("sources = [")]

    assert "-fsanitize=address,undefined" in required_build
    assert "-fno-sanitize-recover=all" in required_build
    assert "-fno-sanitize=" not in required_build


def test_production_and_host_gates_select_the_defined_overlay():
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    conformance = (ROOT / "scripts" / "meshcore_conformance_d1l.py").read_text(
        encoding="utf-8"
    )
    signed_runtime = (
        ROOT / "scripts" / "meshcore_signed_advert_runtime_d1l.py"
    ).read_text(encoding="utf-8")

    for name in ("fe.c", "ge.c", "sc.c"):
        overlay = f"overlays/meshcore_ed25519_defined/{name}"
        upstream = f"third_party/MeshCore/lib/ed25519/{name}"
        assert f'"../{overlay}"' in cmake
        assert f'"../{upstream}"' not in cmake
        overlay_path = ROOT / overlay
        assert overlay_path in conformance_gate.ED25519_ORACLE_SOURCES
        assert overlay_path in signed_runtime_gate.ED25519_C_SOURCES
    assert "-fno-sanitize=shift-base" not in conformance
    assert "-fno-sanitize=shift-base" not in signed_runtime


def test_actions_requires_overlay_validator_with_byte_pinned_clang_18():
    workflow = (ROOT / ".github" / "workflows" / "d1l-ci.yml").read_text(
        encoding="utf-8"
    )
    install = workflow.index("Install pinned Clang sanitizer toolchain")
    validate = workflow.index("Validate Ed25519 defined-arithmetic overlay")
    conformance = workflow.index("Run MeshCore wire-envelope conformance")
    validator_block = workflow[validate:conformance]

    assert install < validate < conformance
    assert "scripts/validate_ed25519_defined_overlay.py" in validator_block
    assert "--cc clang-18" in validator_block
    assert "--sanitizers required" in validator_block
    assert "ed25519_defined_overlay_${D1L_SOURCE_SHA}.json" in validator_block


def test_baseline_and_overlay_are_byte_identical_on_host_corpus(tmp_path):
    compiler = next(
        (candidate for name in ("clang-18", "clang", "gcc", "cc")
         if (candidate := shutil.which(name))),
        None,
    )
    assert compiler is not None, "host differential test requires a C compiler"
    receipt = tmp_path / "ed25519-defined-overlay.json"
    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "validate_ed25519_defined_overlay.py"),
            "--cc",
            compiler,
            "--sanitizers",
            "off",
            "--out",
            str(receipt),
        ],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
        timeout=180,
    )

    assert result.returncode == 0, result.stderr
    payload = json.loads(receipt.read_text(encoding="utf-8"))
    assert payload["differential"] == {
        "deterministic_cases": 256,
        "byte_for_byte_equal": True,
        "output_bytes": 150541,
        "output_sha256": (
            "c37ccb9a5b8a7805dbe0afe44c170eec994e6748f08b2b5296caa67cd8ca4ecb"
        ),
    }
    assert payload["known_answer_tests"] == {
        "RFC8032_section_7_1_test_1_empty_message": "passed",
        "RFC8032_section_7_1_test_2_one_byte_message": "passed",
    }
