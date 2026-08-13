from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_public_release_metadata_has_no_bringup_or_stub_markers():
    config = read("main/d1l_config.h")
    cmake = read("main/CMakeLists.txt")
    console = read("main/comms/usb_console.c")
    mesh_source = read("main/mesh/meshcore_service.c")
    package_script = read("scripts/package_release_d1l.py")

    assert '#define D1L_FIRMWARE_VERSION "1.7.1"' in config
    assert "D1L_PHASE1_BRINGUP" not in cmake
    assert "phase1_stub" not in mesh_source
    assert "phase2_stub" not in console
    assert '\\"meshcore_bridge\\":\\"ready\\"' in console
    assert "current bring-up" not in package_script
