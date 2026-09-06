import pathlib
import re
import shutil
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_radio_callback_converts_driver_snr_to_protocol_units(tmp_path):
    source = (ROOT / "main/mesh/meshcore_service.c").read_text()
    callback = re.search(r"static void on_rx_done\([^)]*\)\s*\{.*?\n\}", source, re.S)
    assert callback
    program = tmp_path / "radio_snr.c"
    program.write_text(
        """
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define D1L_MESHCORE_SERVICE_EVENT_RX_DONE 1
static int8_t observed_snr;
static void enqueue_radio_event(int type, uint8_t *payload, uint16_t size,
                                int16_t rssi, int8_t snr, const void *origin) {
    assert(type == 1 && payload != NULL && size == 1 && rssi == -63);
    assert(origin == NULL);
    observed_snr = snr;
}
"""
        + callback.group(0)
        + """
int main(void) {
    uint8_t packet = 0;
    on_rx_done(&packet, 1, -63, 13);
    assert(observed_snr == 52);  /* 13 dB, not 3.25 dB. */
    on_rx_done(&packet, 1, -63, -2);
    assert(observed_snr == -8);
    on_rx_done(&packet, 1, -63, 32);
    assert(observed_snr == INT8_MAX);
    on_rx_done(&packet, 1, -63, -32);
    assert(observed_snr == INT8_MIN);
    return 0;
}
"""
    )
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required for radio boundary tests"
    binary = tmp_path / "radio_snr"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(program), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
