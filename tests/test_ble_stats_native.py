"""Check the real companion statistics encoder against the documented wire format."""

from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def test_companion_stats_report_radio_values_and_route_counts(tmp_path):
    source = (ROOT / "main/comms/ble_companion_protocol.c").read_text()
    functions = "static uint8_t clamp_u8" + source.split("static uint8_t clamp_u8", 1)[1].split("static uint32_t message_epoch", 1)[0]
    program = tmp_path / "ble_stats.c"
    program.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "mesh/meshcore_service.h"
#define RESP_CODE_STATS 24U
#define STATS_TYPE_CORE 0U
#define STATS_TYPE_RADIO 1U
#define STATS_TYPE_PACKETS 2U
#define ERR_CODE_ILLEGAL_ARG 6U
#define D1L_BLE_PROTOCOL_WIRED_MILLIVOLTS 4200U
static d1l_meshcore_service_status_t provided;
static uint8_t s_pending_payload[512], last_error;
static size_t s_pending_len;
d1l_meshcore_service_status_t d1l_meshcore_service_status(void) {return provided;}
static int64_t esp_timer_get_time(void) {return 3700000;}
static void set_error_response(uint8_t value) {last_error=value;}
static void write_u16_le(uint8_t *p, uint16_t value) {p[0]=value; p[1]=value>>8;}
static void write_u32_le(uint8_t *p, uint32_t value) {
    for (int i=0; i<4; ++i) p[i]=(uint8_t)(value>>(8*i));
}
static uint32_t read_u32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
''' + functions + r'''
int main(void) {
    provided.radio_ready=provided.radio_applied=true;
    provided.runtime_command_queue_depth=2;
    provided.runtime_priority_queue_depth=3;
    provided.runtime_event_queue_depth=4;
    build_stats(STATS_TYPE_CORE);
    assert(s_pending_len==11 && s_pending_payload[0]==24);
    assert((s_pending_payload[2] | s_pending_payload[3]<<8)==4200);
    assert(read_u32(s_pending_payload+4)==3 && s_pending_payload[10]==9);
    provided.radio_noise_floor_dbm=-109;
    provided.radio_last_rssi_dbm=-65;
    provided.radio_last_snr_quarter_db=-8;
    provided.radio_tx_airtime_ms=12500;
    provided.radio_rx_airtime_ms=3500;
    build_stats(STATS_TYPE_RADIO);
    assert(s_pending_len==14 && s_pending_payload[1]==1);
    assert((int16_t)(s_pending_payload[2] | s_pending_payload[3]<<8)==-109);
    assert((int8_t)s_pending_payload[4]==-65 && (int8_t)s_pending_payload[5]==-8);
    assert(read_u32(s_pending_payload+6)==12 && read_u32(s_pending_payload+10)==3);
    provided.radio_last_rssi_dbm=-200;
    build_stats(STATS_TYPE_RADIO);
    assert((int8_t)s_pending_payload[4]==INT8_MIN);
    provided.rx_packets=3; /* Accepted application packets are a separate metric. */
    provided.radio_rx_packets=12;
    provided.tx_packets=7;
    provided.radio_flood_tx=4;
    provided.radio_direct_tx=3;
    provided.radio_flood_rx=10;
    provided.radio_direct_rx=2;
    provided.radio_rx_errors=2;
    build_stats(STATS_TYPE_PACKETS);
    const uint32_t expected[]={12,7,4,3,10,2,2};
    assert(s_pending_len==30 && s_pending_payload[1]==2);
    for (int i=0; i<7; ++i) assert(read_u32(s_pending_payload+2+4*i)==expected[i]);
    build_stats(99);
    assert(last_error==ERR_CODE_ILLEGAL_ARG);
    return 0;
}
''')
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required for companion wire tests"
    binary = tmp_path / "ble_stats"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"),
                    "-I", str(ROOT / "tests/native/stubs"), str(program), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_self_telemetry_uses_local_identity_and_preserves_remote_requests(tmp_path):
    source = (ROOT / "main/comms/ble_companion_protocol.c").read_text()
    function = "static void send_telemetry_command" + source.split(
        "static void send_telemetry_command", 1)[1].split("static void send_binary_command", 1)[0]
    program = tmp_path / "self_telemetry.c"
    program.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define ESP_OK 0
#define ERR_CODE_ILLEGAL_ARG 6U
#define ERR_CODE_BAD_STATE 4U
#define PUSH_CODE_TELEMETRY_RESPONSE 0x8bU
#define D1L_BLE_PROTOCOL_WIRED_MILLIVOLTS 4200U
#define D1L_MESHCORE_ADMIN_QUERY_TELEMETRY 3U
#define D1L_BLE_ADMIN_REQUEST_TELEMETRY 4U
typedef struct {bool identity_ready; uint8_t identity_public_key[32];} d1l_settings_t;
static d1l_settings_t provided;
static int snapshot_error;
static uint8_t reply[32], last_error, remote_key[32];
static size_t reply_length;
static unsigned remote_requests;
static int d1l_settings_public_snapshot(d1l_settings_t *out) {*out=provided; return snapshot_error;}
static void set_error_response(uint8_t error) {last_error=error;}
static bool set_pending(const uint8_t *bytes, size_t length) {
    assert(length<=sizeof(reply)); memcpy(reply,bytes,length); reply_length=length; return true;
}
static void send_contact_telemetry_request(const uint8_t *key, uint8_t mask, bool binary) {
    assert(mask==0 && !binary); memcpy(remote_key,key,32); ++remote_requests;
}
''' + function + r'''
int main(void) {
    uint8_t request[36]={39,0,0,0};
    provided.identity_ready=true;
    for(unsigned i=0;i<32;++i) provided.identity_public_key[i]=(uint8_t)(i+1);
    send_telemetry_command(request,4);
    assert(last_error==0 && remote_requests==0 && reply_length==12);
    assert(reply[0]==0x8b && reply[1]==0 && !memcmp(reply+2,provided.identity_public_key,6));
    assert(reply[8]==1 && reply[9]==116 && reply[10]==1 && reply[11]==164);
    for(unsigned i=0;i<32;++i) request[i+4]=(uint8_t)(i+42);
    send_telemetry_command(request,sizeof(request));
    assert(remote_requests==1 && !memcmp(remote_key,request+4,32));
    const unsigned invalid[]={0,1,2,3,5,35};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        last_error=0; reply_length=0;
        send_telemetry_command(request,invalid[i]);
        assert(last_error==6 && reply_length==0 && remote_requests==1);
    }
    provided.identity_ready=false; last_error=0;
    send_telemetry_command(request,4);
    assert(last_error==4 && reply_length==0 && remote_requests==1);
    provided.identity_ready=true; snapshot_error=1; last_error=0;
    send_telemetry_command(request,4);
    assert(last_error==4 && reply_length==0 && remote_requests==1);
    return 0;
}
''')
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required for companion wire tests"
    binary = tmp_path / "self_telemetry"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", str(program), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
