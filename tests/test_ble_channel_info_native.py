"""Exercise the production channel lookup/encoder with a controlled store."""

import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_phone_finds_free_channel_slots_without_hiding_storage_errors(tmp_path):
    protocol = (ROOT / "main/comms/ble_companion_protocol.c").read_text()
    lookup = "static esp_err_t channel_at_index" + protocol.split(
        "static esp_err_t channel_at_index", 1
    )[1].split("static bool channel_index_for_id", 1)[0]
    encoder = "static void build_channel_info" + protocol.split(
        "static void build_channel_info", 1
    )[1].split("static void build_battery_storage", 1)[0]
    source = tmp_path / "channel_info.c"
    source.write_text(
        r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mesh/channel_store.h"
#define D1L_BLE_PROTOCOL_MAX_CHANNELS 8U
#define RESP_CODE_CHANNEL_INFO 18U
#define ERR_CODE_ILLEGAL_ARG 1U
#define ERR_CODE_BAD_STATE 4U
#define ERR_CODE_UNSUPPORTED_CMD 6U
static d1l_channel_info_t s_channels[D1L_CHANNEL_STORE_CAPACITY];
static d1l_channel_store_stats_t s_channel_stats;
static uint8_t s_pending_payload[512];
static size_t s_pending_len;
static esp_err_t store_result = ESP_OK;
static void set_error_response(uint8_t error) {
    s_pending_payload[0] = 1;
    s_pending_payload[1] = error;
    s_pending_len = 2;
}
static void copy_fixed_text(uint8_t *dest, size_t size, const char *text) {
    memset(dest, 0, size);
    memcpy(dest, text, strlen(text) < size ? strlen(text) : size);
}
static esp_err_t d1l_app_model_copy_channels(
    d1l_channel_info_t *channels, size_t cap, size_t *count,
    uint64_t *active, d1l_channel_store_stats_t *stats) {
    (void)stats;
    assert(cap == D1L_CHANNEL_STORE_CAPACITY);
    if (store_result != ESP_OK) return store_result;
    *count = 1;
    *active = 1;
    channels[0].channel_id = 1;
    strcpy(channels[0].name, "Public");
    return ESP_OK;
}
esp_err_t d1l_channel_store_copy_protocol_key(
    uint64_t id, d1l_channel_protocol_key_t *key) {
    assert(id == 1);
    memset(key, 0, sizeof(*key));
    key->secret_len = 16;
    memset(key->secret, 0xA5, 16);
    return ESP_OK;
}
'''
        + lookup
        + encoder
        + r'''
int main(void) {
    build_channel_info(0);
    assert(s_pending_len == 50 && s_pending_payload[0] == 18);
    assert(strcmp((char *)&s_pending_payload[2], "Public") == 0);
    for (size_t i = 34; i < 50; ++i) assert(s_pending_payload[i] == 0xA5);
    for (uint8_t index = 1; index < 8; ++index) {
        build_channel_info(index);
        assert(s_pending_len == 50 && s_pending_payload[0] == 18);
        assert(s_pending_payload[1] == index);
        for (size_t i = 2; i < 50; ++i) assert(s_pending_payload[i] == 0);
    }
    build_channel_info(8);
    assert(s_pending_len == 2 && s_pending_payload[1] == ERR_CODE_ILLEGAL_ARG);
    store_result = ESP_FAIL;
    build_channel_info(1);
    assert(s_pending_len == 2 && s_pending_payload[1] == ERR_CODE_BAD_STATE);
    puts("channel records: ok");
}
'''
    )
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required for BLE channel tests"
    executable = tmp_path / ("channel_info.exe" if os.name == "nt" else "channel_info")
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/native/stubs"),
         str(source), "-o", str(executable)],
        check=True, capture_output=True, text=True,
    )
    result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    assert result.stdout.strip() == "channel records: ok"
