"""Exercise the production lifecycle with authenticated GATT before CONNECT."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_authenticated_subscription_can_precede_connect_callback(tmp_path):
    source = (ROOT / "main/comms/ble_companion.c").read_text(encoding="utf-8")
    def section(start, end):
        begin = source.index(start)
        return source[begin:source.index(end, begin)]
    functions = section("static bool connection_authorized(", "static int rx_access(")
    functions += section("static void update_security(uint16_t connection_handle)\n{",
                         "static int gap_event(struct ble_gap_event *event, void *arg)\n{")
    functions += "static int dispatch(struct ble_gap_event *event) { switch (event->type) {\n"
    functions += section("    case BLE_GAP_EVENT_CONNECT:", "    case BLE_GAP_EVENT_DISCONNECT:")
    functions += section("    case BLE_GAP_EVENT_SUBSCRIBE:", "    case BLE_GAP_EVENT_MTU:")
    functions += "default: return 0; } }\n"
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define BLE_HS_CONN_HANDLE_NONE UINT16_MAX
#define BLE_HS_EALREADY 2
#define BLE_ERR_REM_USER_CONN_TERM 19
#define D1L_BLE_COMPANION_DEFAULT_ATT_MTU 23
#define D1L_BLE_COMPANION_STATIC_PASSKEY 123456U
enum { D1L_BLE_STATE_PAIRING, D1L_BLE_STATE_CONNECTED, D1L_BLE_STATE_READY };
enum { BLE_GAP_EVENT_CONNECT, BLE_GAP_EVENT_SUBSCRIBE };
typedef struct { uint8_t type, val[6]; } ble_addr_t;
struct ble_gap_conn_desc {
    uint16_t conn_handle;
    ble_addr_t peer_id_addr;
    struct { bool encrypted, authenticated, bonded; } sec_state;
};
struct ble_gap_event {
    int type;
    struct { int status; uint16_t conn_handle; } connect;
    struct { uint16_t attr_handle, conn_handle; bool cur_notify; } subscribe;
};
static struct ble_gap_conn_desc live;
static int s_lock, s_state, security_result;
static bool s_start_requested=true, s_connected, s_encrypted, s_authenticated;
static bool s_bonded, s_notification_requested, s_notification_enabled;
static bool s_tx_busy, s_peer_known, s_advertising=true;
static uint16_t s_connection_handle=BLE_HS_CONN_HANDLE_NONE, s_att_mtu=23;
static uint16_t s_tx_value_handle=18;
static uint32_t s_pairing_passkey, s_connect_count, s_security_reject_count;
static uint32_t s_rx_drop_count, s_tx_drop_count;
static int64_t s_last_tx_started_us;
static ble_addr_t s_peer_id_addr;
typedef struct { size_t count; } queue_t;
static queue_t s_rx_queue, s_tx_queue;
static unsigned pumps, security_starts, terminated, errors;
static size_t d1l_ble_companion_queue_clear(queue_t *queue) {
    size_t count=queue->count; queue->count=0; return count;
}
static int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *out) {
    if(handle!=live.conn_handle) return 1;
    *out=live; return 0;
}
static uint16_t ble_att_mtu(uint16_t handle) { assert(handle==7); return 517; }
static void pump_tx(void) { ++pumps; }
static int start_advertising(void) { return 0; }
static void note_nimble_error(int error) { assert(error); ++errors; }
static int ble_gap_security_initiate(uint16_t handle) {
    assert(handle==7); ++security_starts; return security_result;
}
static int ble_gap_terminate(uint16_t handle, int reason) {
    assert(handle==7 && reason==BLE_ERR_REM_USER_CONN_TERM); ++terminated; return 0;
}
''' + functions + r'''
int main(void) {
    live.conn_handle=7;
    live.sec_state.encrypted=live.sec_state.authenticated=live.sec_state.bonded=true;
    s_rx_queue.count=2; s_tx_queue.count=3;
    struct ble_gap_event subscribe={.type=BLE_GAP_EVENT_SUBSCRIBE,
        .subscribe={.attr_handle=18,.conn_handle=7,.cur_notify=true}};
    dispatch(&subscribe);
    assert(s_connected && s_connection_handle==7 && s_att_mtu==517);
    assert(s_encrypted && s_authenticated && s_bonded && s_notification_enabled);
    assert(s_state==D1L_BLE_STATE_READY && s_connect_count==1 && pumps==1);
    assert(s_rx_drop_count==2 && s_tx_drop_count==3);
    assert(s_peer_known && !s_advertising);
    s_rx_queue.count=4; s_tx_queue.count=5; s_tx_busy=true;
    struct ble_gap_event connect={.type=BLE_GAP_EVENT_CONNECT,.connect={.conn_handle=7}};
    dispatch(&connect);
    assert(s_connect_count==1 && s_notification_enabled && s_tx_busy);
    assert(s_rx_queue.count==4 && s_tx_queue.count==5);
    assert(security_starts==0 && terminated==0);
    reset_connection_locked(); s_start_requested=false;
    dispatch(&subscribe);
    assert(!s_connected && !s_notification_requested && !s_notification_enabled);
    s_start_requested=true;
    live.sec_state.encrypted=live.sec_state.authenticated=live.sec_state.bonded=false;
    security_result=BLE_HS_EALREADY;
    dispatch(&connect);
    assert(s_connected && s_state==D1L_BLE_STATE_PAIRING);
    assert(security_starts==1 && terminated==0 && errors==0 && s_security_reject_count==0);
    live.sec_state.encrypted=true; live.sec_state.bonded=true;
    update_security(7);
    assert(terminated==1 && s_security_reject_count==1 && !s_notification_enabled);
    subscribe.subscribe.conn_handle=99;
    dispatch(&subscribe);
    assert(s_connection_handle==7 && !s_notification_enabled);
    return 0;
}
'''
    path = tmp_path / "connection_order.c"
    path.write_text(code, encoding="utf-8")
    binary = tmp_path / ("connection_order.exe" if os.name == "nt" else "connection_order")
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required for the BLE lifecycle test"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path), "-o", str(binary)], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
