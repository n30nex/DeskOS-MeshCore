"""Verify real companion SENT/ACK bytes against the radio delivery session."""

import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_phone_delivery_confirmation_waits_for_matching_radio_ack(tmp_path):
    protocol = (ROOT / "main/comms/ble_companion_protocol.c").read_text()
    functions = "static void set_dm_sent_response" + protocol.split(
        "static void set_dm_sent_response", 1
    )[1].split("static void send_dm_command", 1)[0]
    source = tmp_path / "dm_confirmation.c"
    source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mesh/dm_store.h"
#include "mesh/meshcore_dm_retry.h"
#define RESP_CODE_SENT 6U
#define PUSH_CODE_SEND_CONFIRMED 0x82U
static uint64_t s_phone_dm_session;
static uint32_t s_phone_dm_ack;
static uint32_t s_phone_dm_started_ms;
static uint8_t s_pending_payload[512];
static size_t s_pending_len;
static d1l_dm_entry_t s_dms[16], stored;
static int64_t now_us = 1000000;
static int64_t esp_timer_get_time(void) { return now_us; }
static void write_u32_le(uint8_t *out, uint32_t v) {
    for (unsigned i=0; i<4; ++i) out[i] = (uint8_t)(v >> (8*i));
}
static uint32_t read_u32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static bool set_pending(const uint8_t *p, size_t n) {
    if (s_pending_len) return false;
    memcpy(s_pending_payload,p,n); s_pending_len=n; return true;
}
bool d1l_dm_store_find_delivery_session(uint64_t id, d1l_dm_entry_t *out) {
    if (id != stored.delivery_session_id) return false;
    *out=stored; return true;
}
''' + functions + r'''
int main(void) {
    stored.delivery_session_id=7;
    stored.ack_hash=0x11223344;
    stored.delivery_state=D1L_DM_DELIVERY_AWAITING_ACK;
    stored.delivered=true;
    set_dm_sent_response(&stored, true);
    assert(s_pending_len==10 && s_pending_payload[0]==6 && s_pending_payload[1]==1);
    assert(read_u32(s_pending_payload+2)==0x11223344);
    assert(read_u32(s_pending_payload+6)==55000);
    s_pending_len=0;
    maybe_queue_dm_confirmation();
    assert(s_pending_len==0 && s_phone_dm_session==7);
    stored.ack_hash=0x55667788; /* A retry has its own on-air ACK hash. */
    stored.acked=true;
    stored.delivery_state=D1L_DM_DELIVERY_ACKNOWLEDGED;
    now_us=3500000;
    s_pending_len=1; /* Keep the confirmation while another reply is staged. */
    maybe_queue_dm_confirmation();
    assert(s_phone_dm_session==7);
    s_pending_len=0;
    maybe_queue_dm_confirmation();
    assert(s_pending_len==9 && s_pending_payload[0]==0x82);
    assert(read_u32(s_pending_payload+1)==0x11223344);
    assert(read_u32(s_pending_payload+5)==2500);
    s_pending_len=0;
    maybe_queue_dm_confirmation();
    assert(s_pending_len==0);
    set_dm_sent_response(&stored, false);
    assert(s_pending_payload[1]==0);
    s_pending_len=0;
    stored.acked=false;
    stored.delivery_state=D1L_DM_DELIVERY_FAILED_RADIO;
    maybe_queue_dm_confirmation();
    assert(s_pending_len==0 && s_phone_dm_session==0);
    set_dm_sent_response(&stored, true);
    s_pending_len=0;
    stored.delivery_session_id=8;
    stored.acked=true;
    stored.delivery_state=D1L_DM_DELIVERY_ACKNOWLEDGED;
    maybe_queue_dm_confirmation();
    assert(s_pending_len==0 && s_phone_dm_session==0);
    puts("phone delivery confirmation: ok");
}
''')
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required for BLE delivery tests"
    executable = tmp_path / ("dm_confirmation.exe" if os.name == "nt" else "dm_confirmation")
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/native/stubs"),
         str(source), str(ROOT / "main/mesh/dm_delivery_state.c"),
         "-o", str(executable)], check=True, capture_output=True, text=True,
    )
    result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    assert result.stdout.strip() == "phone delivery confirmation: ok"
