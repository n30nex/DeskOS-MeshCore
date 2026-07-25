import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_prefetch_plan_is_registered_and_bounded():
    cmake = read("main/CMakeLists.txt")
    header = read("main/map/map_prefetch_plan.h")
    source = read("main/map/map_prefetch_plan.c")

    assert '"map/map_prefetch_plan.c"' in cmake
    assert "D1L_MAP_PREFETCH_NODE_RADIUS_KM 200.0" in header
    assert "D1L_MAP_PREFETCH_CARD_RESERVE_KB" in header
    assert "D1L_MAP_PREFETCH_CARD_ALLOCATION_PERCENT 60U" in header
    assert "distance_km(center_geographic_latitude" in source
    assert "wrap_delta_longitude" in source
    assert "candidate_bytes > out_plan->allocation_bytes" in source
    assert "d1l_map_prefetch_plan_tile_at" in source


def test_prefetch_plan_native_vectors(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for map prefetch vectors")

    executable = tmp_path / "map_prefetch_plan_test"
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main/map/map_prefetch_plan.c"),
        str(ROOT / "tests/native/map_prefetch_plan_test.c"),
        "-lm",
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    subprocess.run(
        [str(executable)], cwd=ROOT, check=True, capture_output=True, text=True
    )
