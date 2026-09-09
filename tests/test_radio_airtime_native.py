"""Exercise the vendor airtime calculation and actual radio/watchdog setup."""

from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def c_function(source, name):
    match = re.search(r"(?:static\s+)?(?:uint32_t|bool|esp_err_t)\s+" + name + r"\([^)]*\)\s*\{", source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def test_valid_slow_frames_fit_hardware_and_owner_deadlines(tmp_path):
    bsp = ROOT / "third_party/sensecap_indicator_esp32"
    # Start from pinned vendor bytes even if the build worktree is patched.
    original = subprocess.check_output(["git", "show", "HEAD:components/lora/radio.c"], cwd=bsp)
    target = tmp_path / "components/lora/radio.c"
    target.parent.mkdir(parents=True)
    target.write_bytes(original)
    subprocess.run(["git", "apply", "--unidiff-zero", "--ignore-space-change",
                    str(ROOT / "patches/sensecap_indicator_airtime.patch")], cwd=tmp_path, check=True)
    driver = target.read_text()
    service = (ROOT / "main/mesh/meshcore_service.c").read_text()
    bandwidths = re.search(r"const RadioLoRaBandwidths_t Bandwidths\[\]\s*=\s*\{[^}]+\};", driver)
    assert bandwidths
    constants = "\n".join(re.findall(
        r"^#define D1L_MESHCORE_(?:TX_TIMEOUT_MS|TX_AIRTIME_MARGIN_MS|TX_WATCHDOG_GRACE_MS|PREAMBLE_LOW_SF|BW_INDEX_62K5) .*$",
        service, re.M))
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "mesh/dm_delivery_state.h"
#include "mesh/meshcore_runtime_guard.h"
#include "mesh/meshcore_wire.h"
typedef enum {MODEM_FSK, MODEM_LORA} RadioModems_t;
typedef enum {LORA_BW_007, LORA_BW_010, LORA_BW_015, LORA_BW_020, LORA_BW_031,
              LORA_BW_041, LORA_BW_062, LORA_BW_125, LORA_BW_250, LORA_BW_500} RadioLoRaBandwidths_t;
typedef enum {RADIO_ADDRESSCOMP_FILT_OFF} RadioAddressComp_t;
typedef struct {uint32_t frequency_hz; float bandwidth_khz;
                uint8_t spreading_factor, coding_rate; int8_t tx_power_dbm;} d1l_radio_profile_t;
#define D1L_MESHCORE_SERVICE_RADIO_ERROR 4
static struct {int state;} s_status;
static uint32_t s_radio_tx_timeout_ms, programmed_timeout, s_next_radio_tx_operation_id;
static d1l_mesh_tx_operation_identity_t s_active_radio_tx;
static d1l_mesh_tx_watchdog_t s_radio_tx_watchdog;
static void status_lock(void) {}
static void status_unlock(void) {}
static uint32_t bsp_sx126x_fault_get(void) {return 0U;}
static int64_t esp_timer_get_time(void) {return 1000000;}
static void publish_callback_tx_operation(const d1l_mesh_tx_operation_identity_t *identity) {(void)identity;}
static void channel(uint32_t hz) {(void)hz;}
static void network(bool enabled) {(void)enabled;}
static void rx_config(RadioModems_t modem, ...) {(void)modem;}
static void tx_config(RadioModems_t modem, int8_t power, uint32_t fdev, uint32_t bw,
                      uint32_t sf, uint8_t cr, uint16_t preamble, bool fix, bool crc,
                      bool hopping, uint8_t period, bool iq, uint32_t timeout) {
    programmed_timeout = timeout;
}
static void apply_sx1262_lora_params(const d1l_radio_profile_t *profile,
                                    RadioLoRaBandwidths_t bw, uint8_t cr) {}
'''
    code += constants + "\n" + bandwidths.group(0) + "\n"
    for name in ["RadioGetLoRaBandwidthInHz", "RadioGetGfskTimeOnAirNumerator", "RadioGetLoRaTimeOnAirNumerator", "RadioTimeOnAir"]:
        code += c_function(driver, name) + "\n"
    code += r'''
static const struct {
  uint32_t (*TimeOnAir)(RadioModems_t,uint32_t,uint32_t,uint8_t,uint16_t,bool,uint8_t,bool);
  void (*SetChannel)(uint32_t);
  void (*SetPublicNetwork)(bool);
  void (*SetRxConfig)(RadioModems_t,...);
  void (*SetTxConfig)(RadioModems_t,int8_t,uint32_t,uint32_t,uint32_t,uint8_t,uint16_t,bool,bool,bool,uint8_t,bool,uint32_t);
} Radio = {RadioTimeOnAir, channel, network, rx_config, tx_config};
'''
    for name in ["bandwidth_to_driver_index", "coding_rate_to_driver_value", "configure_radio_profile", "meshcore_radio_tx_operation_begin"]:
        code += c_function(service, name) + "\n"
    code += r'''
int main(void) {
  assert(RadioTimeOnAir(MODEM_LORA, 0, 7, 1, 32, false, 176, true) == 307);
  assert(RadioTimeOnAir(MODEM_LORA, 0, 12, 1, 32, false, 176, true) == 7349);
  assert(RadioTimeOnAir(MODEM_LORA, 3, 7, 1, 32, false, 176, true) == 613);
  assert(RadioTimeOnAir(MODEM_LORA, 3, 10, 1, 32, false, 176, true) == 4412);
  d1l_radio_profile_t profile = {910525000, 62.5f, 12, 8, 20};
  assert(configure_radio_profile(&profile) == ESP_OK);
  assert(programmed_timeout == 30639 && s_radio_tx_timeout_ms == 30639);
  assert(meshcore_radio_tx_operation_begin(D1L_MESH_TX_OPERATION_GENERIC, NULL));
  assert(s_radio_tx_watchdog.deadline_us == 1000000ULL + (30639ULL + 250ULL) * 1000ULL);
  profile.spreading_factor = 7;
  profile.coding_rate = 5;
  assert(configure_radio_profile(&profile) == ESP_OK);
  assert(programmed_timeout == 5000);
  profile.spreading_factor = 13;
  assert(configure_radio_profile(&profile) == ESP_ERR_NOT_SUPPORTED);
  return 0;
}
'''
    program = tmp_path / "radio_airtime.c"
    program.write_text(code)
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required for radio timing tests"
    binary = tmp_path / "radio_airtime"
    subprocess.run([compiler, "-std=c11", "-O2", "-I", str(ROOT / "main"),
                    "-I", str(ROOT / "tests/native/stubs"), str(program), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
