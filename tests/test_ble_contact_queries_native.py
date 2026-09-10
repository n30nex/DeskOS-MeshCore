"""Execute the production phone discovery/telemetry adapters with real frames."""

import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_phone_contact_queries_preserve_tags_payloads_and_session_boundaries(tmp_path):
    protocol = (ROOT / "main/comms/ble_companion_protocol.c").read_text(encoding="utf-8")
    telemetry = "static void send_contact_telemetry_request" + protocol.split(
        "static void send_contact_telemetry_request", 1
    )[1].split("static void send_telemetry_command", 1)[0]
    discovery = "static void send_control_command" + protocol.split(
        "static void send_control_command", 1
    )[1].split("static void build_autoadd_config", 1)[0]
    sent = "static void set_admin_sent_response" + protocol.split(
        "static void set_admin_sent_response", 1
    )[1].split("static uint32_t message_epoch", 1)[0]
    radio = "static void set_radio_command" + protocol.split(
        "static void set_radio_command", 1
    )[1].split("static void set_tx_power_command", 1)[0]
    source = tmp_path / "phone_queries.c"
    source.write_text(r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "mesh/contact_store.h"
#include "mesh/meshcore_service.h"
#include "app/app_model.h"
#define RESP_CODE_SENT 6U
#define ERR_CODE_NOT_FOUND 2U
#define ERR_CODE_UNSUPPORTED_CMD 1U
#define ERR_CODE_ILLEGAL_ARG 6U
#define PUSH_CODE_TELEMETRY_RESPONSE 0x8bU
#define PUSH_CODE_BINARY_RESPONSE 0x8cU
#define PUSH_CODE_CONTROL_DATA 0x8eU
#define D1L_BLE_PROTOCOL_ADMIN_TIMEOUT_MS 60000U
#define D1L_BLE_ADMIN_REQUEST_BINARY_QUERY 5
#define D1L_BLE_ADMIN_REQUEST_TELEMETRY 3
static uint8_t s_pending_payload[512];
static size_t s_pending_len;
static uint32_t s_phone_telemetry_tag, s_phone_discovery_tag, s_phone_discovery_sent;
static bool s_phone_telemetry_binary;
static uint8_t s_phone_telemetry_public_key[6];
static char s_phone_telemetry_fingerprint[D1L_NODE_FINGERPRINT_LEN];
static d1l_meshcore_contact_telemetry_snapshot_t s_phone_telemetry_snapshot, telemetry_result;
static d1l_meshcore_discovery_snapshot_t s_phone_discovery_snapshot, discovery_result;
static const uint8_t peer_key[32] = {0x02, 0x49, 0x99, 0xde, 0xdf, 0xd2, 0x67, 0x63};
static esp_err_t request_result = ESP_OK, last_result;
static uint8_t last_error_code, last_mask, last_control[10];
static unsigned telemetry_calls, discovery_calls;
static uint32_t next_tag = 0x11223344;
static bool contact_allowed = true;
static bool admin_contact;
static unsigned admin_calls;
static unsigned radio_saves;
static d1l_app_radio_profile_edit_t radio_profile;
static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void write_u32_le(uint8_t *p, uint32_t v) {
    for (unsigned i=0; i<4; ++i) p[i]=(uint8_t)(v>>(8*i));
}
static bool decode_hex(const char *text, size_t bytes, uint8_t *out) {
    if (strlen(text) != 2*bytes) return false;
    for (size_t i=0; i<bytes; ++i) {
        unsigned value;
        if (sscanf(text+2*i, "%2x", &value) != 1) return false;
        out[i]=(uint8_t)value;
    }
    return true;
}
static void set_error_response(uint8_t code) { last_error_code=code; }
static void set_result_response(esp_err_t result) { last_result=result; }
static bool set_pending(const uint8_t *p, size_t n) {
    assert(n<=sizeof(s_pending_payload));
    memcpy(s_pending_payload,p,n); s_pending_len=n; return true;
}
static bool contact_for_public_key(const uint8_t *key, d1l_contact_entry_t *out) {
    if (memcmp(key,peer_key,32)) return false;
    memset(out,0,sizeof(*out)); strcpy(out->fingerprint,"024999dedfd26763"); return true;
}
bool d1l_contact_store_can_path_probe(const d1l_contact_entry_t *contact) {
    (void)contact; return contact_allowed;
}
bool d1l_contact_store_can_admin(const d1l_contact_entry_t *contact) {
    (void)contact; return admin_contact;
}
static void begin_admin_query_command(const uint8_t *key,
    d1l_meshcore_admin_query_t query, uint16_t offset, int kind) {
    assert(!memcmp(key,peer_key,32) && query==D1L_MESHCORE_ADMIN_QUERY_TELEMETRY);
    assert(offset==0 && kind==D1L_BLE_ADMIN_REQUEST_TELEMETRY); admin_calls++;
}
esp_err_t d1l_meshcore_service_request_contact_telemetry(const char *fingerprint,
    uint8_t inverse_mask, uint32_t *out_tag) {
    assert(!strcmp(fingerprint,"024999dedfd26763"));
    telemetry_calls++; last_mask=inverse_mask;
    *out_tag=request_result==ESP_OK ? next_tag++ : 0U; return request_result;
}
void d1l_meshcore_service_contact_telemetry_snapshot(const char *fingerprint,
    d1l_meshcore_contact_telemetry_snapshot_t *out) {
    assert(!strcmp(fingerprint,"024999dedfd26763")); *out=telemetry_result;
}
esp_err_t d1l_meshcore_service_request_discovery(const uint8_t *request, size_t length) {
    assert(d1l_meshcore_discovery_request_valid(request,length));
    discovery_calls++; memset(last_control,0,sizeof(last_control));
    memcpy(last_control,request,length); return request_result;
}
void d1l_meshcore_service_discovery_snapshot(d1l_meshcore_discovery_snapshot_t *out) {
    *out=discovery_result;
}
void d1l_app_model_current_radio_profile(d1l_app_radio_profile_edit_t *out) {
    *out=radio_profile;
}
esp_err_t d1l_app_model_save_radio_profile(const d1l_app_radio_profile_edit_t *profile) {
    radio_saves++; radio_profile=*profile; return ESP_OK;
}
''' + sent + telemetry + discovery + radio + r'''
int main(void) {
    const uint8_t lpp[] = {1,116,1,164};
    uint8_t wrong_key[32]={1};
    send_contact_telemetry_request(wrong_key,0,false);
    assert(last_error_code==ERR_CODE_NOT_FOUND && telemetry_calls==0);
    contact_allowed=false;
    send_contact_telemetry_request(peer_key,0,false);
    assert(telemetry_calls==0);
    contact_allowed=true; admin_contact=true;
    send_contact_telemetry_request(peer_key,0,false);
    assert(admin_calls==1 && telemetry_calls==0);
    admin_contact=false; request_result=ESP_ERR_TIMEOUT;
    send_contact_telemetry_request(peer_key,0,false);
    assert(last_result==ESP_ERR_TIMEOUT && s_phone_telemetry_tag==0);
    request_result=ESP_OK;
    send_contact_telemetry_request(peer_key,0xfe,false);
    assert(last_mask==0xfe && s_pending_len==10 && s_pending_payload[0]==RESP_CODE_SENT);
    assert(s_pending_payload[1]==1 && read_u32_le(s_pending_payload+2)==s_phone_telemetry_tag);
    assert(read_u32_le(s_pending_payload+6)==60000);
    telemetry_result.pending=true; telemetry_result.pending_tag=s_phone_telemetry_tag;
    maybe_queue_contact_telemetry(); /* Never overwrite the initial SENT frame. */
    assert(s_pending_len==10);
    s_pending_len=0;
    telemetry_result.history_count=1;
    telemetry_result.history[0].tag=s_phone_telemetry_tag-1;
    maybe_queue_contact_telemetry();
    assert(s_pending_len==0 && s_phone_telemetry_tag!=0);
    telemetry_result.history[0].tag=s_phone_telemetry_tag;
    telemetry_result.history[0].wire_len=sizeof(lpp);
    memcpy(telemetry_result.history[0].wire,lpp,sizeof(lpp));
    maybe_queue_contact_telemetry();
    assert(s_pending_len==12 && s_pending_payload[0]==0x8b && s_pending_payload[1]==0);
    assert(!memcmp(s_pending_payload+2,peer_key,6) && !memcmp(s_pending_payload+8,lpp,4));
    assert(s_phone_telemetry_tag==0);
    s_pending_len=0; maybe_queue_contact_telemetry(); assert(s_pending_len==0);
    send_contact_telemetry_request(peer_key,0,true);
    const uint32_t binary_tag=s_phone_telemetry_tag;
    s_pending_len=0; telemetry_result.history[0].tag=binary_tag;
    maybe_queue_contact_telemetry();
    assert(s_pending_len==10 && s_pending_payload[0]==0x8c);
    assert(read_u32_le(s_pending_payload+2)==binary_tag && !memcmp(s_pending_payload+6,lpp,4));
    s_pending_len=0; send_contact_telemetry_request(peer_key,0,true);
    s_pending_len=0; telemetry_result.history[0].tag=s_phone_telemetry_tag;
    telemetry_result.history[0].wire_len=sizeof(telemetry_result.history[0].wire)+1;
    maybe_queue_contact_telemetry(); assert(s_pending_len==0 && s_phone_telemetry_tag==0);
    send_contact_telemetry_request(peer_key,0,false); s_pending_len=0;
    memset(&telemetry_result,0,sizeof(telemetry_result));
    maybe_queue_contact_telemetry(); assert(s_phone_telemetry_tag==0 && s_pending_len==0);

    uint8_t request[]={55,0x81,4,0x44,0x33,0x22,0x11};
    send_control_command(request,sizeof(request));
    assert(last_result==ESP_OK && discovery_calls==1);
    assert(!memcmp(last_control,request+1,sizeof(request)-1));
    assert(s_phone_discovery_tag==0x11223344 && s_phone_discovery_sent==0);
    request[1]=0x90; send_control_command(request,sizeof(request));
    assert(last_error_code==ERR_CODE_ILLEGAL_ARG && discovery_calls==1);
    discovery_result.tag=s_phone_discovery_tag; discovery_result.active=true;
    discovery_result.result_count=2;
    strcpy(discovery_result.results[0].public_key_hex,
        "024999dedfd26763000000000000000000000000000000000000000000000000");
    discovery_result.results[0].node_type=2;
    discovery_result.results[0].local_snr_quarter_db=-9;
    discovery_result.results[0].last_rssi_dbm=-150;
    discovery_result.results[0].remote_snr_quarter_db=16;
    strcpy(discovery_result.results[1].public_key_hex,"1122334455667788");
    discovery_result.results[1].node_type=4;
    maybe_queue_discovery_results();
    assert(s_pending_len==42 && s_pending_payload[0]==0x8e);
    assert((int8_t)s_pending_payload[1]==-9 && (int8_t)s_pending_payload[2]==-128);
    assert(s_pending_payload[3]==0 && s_pending_payload[4]==0x92 && s_pending_payload[5]==16);
    assert(read_u32_le(s_pending_payload+6)==0x11223344);
    assert(!memcmp(s_pending_payload+10,peer_key,32));
    maybe_queue_discovery_results(); assert(s_pending_len==42);
    s_pending_len=0; maybe_queue_discovery_results();
    assert(s_pending_len==18 && s_pending_payload[4]==0x94 && s_pending_payload[10]==0x11);
    s_pending_len=0; maybe_queue_discovery_results(); assert(s_pending_len==0);
    discovery_result.tag++; maybe_queue_discovery_results();
    assert(s_phone_discovery_tag==0 && s_pending_len==0);
    uint8_t radio_command[13]={11};
    write_u32_le(radio_command+1,910525);
    write_u32_le(radio_command+5,62500);
    radio_command[9]=7; radio_command[10]=5;
    set_radio_command(radio_command,12);
    assert(radio_saves==1 && radio_profile.frequency_hz==910525000);
    assert(radio_profile.bandwidth_tenths_khz==625);
    radio_command[11]=1;
    set_radio_command(radio_command,12);
    assert(radio_saves==1 && last_error_code==ERR_CODE_UNSUPPORTED_CMD);
    radio_command[11]=0;
    set_radio_command(radio_command,13);
    assert(radio_saves==1 && last_error_code==ERR_CODE_ILLEGAL_ARG);
    puts("phone contact queries: ok");
    return 0;
}
''', encoding="utf-8")
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A native C compiler is required"
    executable = tmp_path / ("phone_queries.exe" if os.name == "nt" else "phone_queries")
    subprocess.run([compiler, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "tests/native/stubs"), "-I", str(ROOT / "main"),
                    str(source), "-o", str(executable)], check=True, capture_output=True, text=True)
    completed = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    assert completed.stdout.strip() == "phone contact queries: ok"
