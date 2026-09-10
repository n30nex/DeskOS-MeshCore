"""Exercise actual message encoders with retained zero/multihop receive paths."""

import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_phone_received_messages_report_stored_paths(tmp_path):
    protocol = (ROOT / "main/comms/ble_companion_protocol.c").read_text()
    start = protocol.index("static uint8_t received_flood_path_len")
    end = protocol.index("static void build_next_message", start)
    source = tmp_path / "received_paths.c"
    source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mesh/channel_store.h"
#include "mesh/dm_store.h"
#include "comms/ble_companion_protocol.h"
#define RESP_CODE_CONTACT_MSG_RECV 7
#define RESP_CODE_CHANNEL_MSG_RECV 8
#define RESP_CODE_CONTACT_MSG_RECV_V3 16
#define RESP_CODE_CHANNEL_MSG_RECV_V3 17
static d1l_message_entry_t s_messages[D1L_MESSAGE_STORE_CAPACITY], message;
static d1l_dm_entry_t s_dms[D1L_DM_STORE_CAPACITY], dm;
static d1l_channel_info_t s_channels[D1L_CHANNEL_STORE_CAPACITY];
static d1l_ble_companion_protocol_status_t s_status;
static uint8_t s_pending_payload[512];
static size_t s_pending_len;
static uint32_t s_last_synced_message_seq, s_last_synced_dm_seq;
size_t d1l_dm_store_copy_recent(d1l_dm_entry_t *out, size_t cap) {
    assert(cap == D1L_DM_STORE_CAPACITY); out[0] = dm; return 1;
}
bool d1l_contact_store_find_by_fingerprint(const char *key, d1l_contact_entry_t *out) {
    assert(!strcmp(key, "0001020304050607"));
    memset(out, 0, sizeof(*out));
    strcpy(out->public_key_hex, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    return true;
}
static bool decode_hex(const char *hex, size_t size, uint8_t *out) {
    for (size_t i = 0; i < size; ++i) {
        unsigned byte; assert(sscanf(hex + 2*i, "%2x", &byte) == 1);
        out[i] = (uint8_t)byte;
    }
    return true;
}
esp_err_t d1l_message_store_snapshot_retained(d1l_message_entry_t *out, size_t cap,
    size_t *count, d1l_message_retained_snapshot_t *snapshot) {
    (void)snapshot; assert(cap == D1L_MESSAGE_STORE_CAPACITY);
    out[0] = message; *count = 1; return ESP_OK;
}
static bool channel_index_for_id(uint64_t id, uint8_t *out) {
    assert(id == 42); *out = 1; return true;
}
static uint32_t message_epoch(uint32_t uptime) { return uptime + 1700000000U; }
uint32_t d1l_message_entry_display_timestamp(const d1l_message_entry_t *entry) {
    (void)entry; return 1700000123U;
}
static int8_t snr_tenths_to_quarter_db(int value) { return (int8_t)(value*4/10); }
static void write_u32_le(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8*i));
}
''' + protocol[start:end] + r'''
int main(void) {
    const uint8_t widths[] = {0, 1, 1, 1, 2, 3, 1};
    const uint8_t hops[] =   {0, 0, 1, 4, 3, 2, 63};
    const uint8_t paths[] =  {0, 0, 1, 4, 0x43, 0x82, 63};
    strcpy(message.direction, "rx");
    strcpy(message.author, "Nearby"); strcpy(message.text, "test");
    message.channel_id = 42; message.snr_tenths = 50;
    strcpy(s_channels[1].name, "Test");
    strcpy(dm.direction, "rx"); strcpy(dm.text, "reply");
    strcpy(dm.contact_fingerprint, "0001020304050607");
    dm.snr_tenths = -25;
    for (uint8_t version = 2; version <= 3; ++version) {
        s_status.client_protocol_version = version;
        const unsigned header = version == 3 ? 4 : 1;
        for (size_t i = 0; i < sizeof(hops); ++i) {
            message.seq++; message.path_hash_bytes = widths[i]; message.path_hops = hops[i];
            assert(build_next_channel_message());
            assert(s_pending_payload[0] == (version == 3 ? 17 : 8));
            assert(s_pending_payload[header] == 1);
            assert(s_pending_payload[header + 1] == paths[i]);
            assert(s_pending_payload[header + 2] == 0);
            assert(s_pending_len == header + 7 + strlen("Nearby: test"));
            assert(!memcmp(s_pending_payload + header + 7, "Nearby: test", 12));
            assert(!build_next_channel_message());
            dm.seq++; dm.path_hash_bytes = widths[i]; dm.path_hops = hops[i];
            assert(build_next_dm_message());
            assert(s_pending_payload[0] == (version == 3 ? 16 : 7));
            for (unsigned j = 0; j < 6; ++j) assert(s_pending_payload[header + j] == j);
            assert(s_pending_payload[header + 6] == (hops[i] == 0 ? 0xff : paths[i]));
            assert(s_pending_payload[header + 7] == 0);
            assert(!memcmp(s_pending_payload + header + 12, "reply", 5));
            assert(!build_next_dm_message());
        }
    }
    puts("received message paths: ok");
}
''')
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required"
    executable = tmp_path / ("received_paths.exe" if os.name == "nt" else "received_paths")
    subprocess.run([compiler, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "tests/native/stubs"), "-I", str(ROOT / "main"),
                    str(source), "-o", str(executable)], check=True, capture_output=True, text=True)
    result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    assert result.stdout.strip() == "received message paths: ok"
