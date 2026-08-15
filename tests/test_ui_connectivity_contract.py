from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_connectivity_truth_is_owned_outside_the_ui_task_monolith():
    cmake = read("main/CMakeLists.txt")
    phase1 = read("main/ui/ui_phase1.c")
    header = read("main/ui/ui_connectivity.h")
    source = read("main/ui/ui_connectivity.c")

    assert '"ui/ui_connectivity.c"' in cmake
    assert '#include "ui_connectivity.h"' in phase1
    assert "d1l_ui_wifi_view_model_t" in header
    assert "d1l_ui_ble_view_model_t" in header
    assert "d1l_ui_connectivity_wifi_view" in phase1
    assert "d1l_ui_connectivity_ble_view" in phase1
    assert '"Scan to list nearby 2.4 GHz networks"' in source
    assert '"Bluetooth companion connections are unavailable."' in source
    assert '"USB stays available for recovery and diagnostics."' in source
    assert '"Using the MeshCore connection alongside Wi-Fi."' in source
    assert '"Enter PIN %06lu on the companion device."' in source

    assert "lv_" not in source
    assert "d1l_app_model_" not in source
    assert "freertos/" not in source
    assert "esp_netif" not in source
    assert "request_content_refresh" not in source
