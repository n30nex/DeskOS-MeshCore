import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_production_node_and_contact_store_native_behavior(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for native node/contact store tests")

    executable = tmp_path / ("store_behavior_test.exe" if os.name == "nt" else "store_behavior_test")
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DD1L_CONTACT_STORE_TEST_HOOKS",
        "-I",
        str(ROOT / "tests/native/stubs"),
        "-I",
        str(ROOT / "tests/native"),
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main/mesh/node_store.c"),
        str(ROOT / "main/mesh/contact_store.c"),
        str(ROOT / "main/mesh/meshcore_advert_admission.c"),
        str(ROOT / "main/mesh/meshcore_packet_hash.c"),
        str(ROOT / "main/mesh/meshcore_path_state.c"),
        str(ROOT / "main/mesh/meshcore_wire.c"),
        str(ROOT / "main/mesh/contact_uri.c"),
        str(ROOT / "tests/native/esp_nvs_stubs.c"),
        str(ROOT / "tests/native/retained_blob_store_nvs_adapter.c"),
        str(ROOT / "tests/native/mbedtls_md_stub.c"),
        str(ROOT / "tests/native/store_behavior_test.c"),
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    completed = subprocess.run(
        [str(executable)], cwd=ROOT, check=True, capture_output=True, text=True
    )
    assert completed.stdout.strip() == "native node/contact store behavior: ok"
