import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = re.search(r"^(?:static )?(?:bool|void|esp_err_t) " + name + r"\([^;]+?\)\s*\{", source, re.M).start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_observer_clients_drain_before_wifi_teardown_and_cannot_restart(tmp_path):
    observer = (ROOT / "main/comms/observer_manager.c").read_text(encoding="utf-8")
    connectivity = (ROOT / "main/comms/connectivity_manager.c").read_text(encoding="utf-8")
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#define D1L_OBSERVER_BROKER_COUNT 3U
#define D1L_WIFI_NETWORK_QUIESCE_TIMEOUT_MS 20000U
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
typedef struct {
    void *client; bool connected; uint32_t token_issued_at, inflight_sequence;
    int inflight_message_id; esp_err_t last_error;
    char username[32], password[32];
} d1l_observer_endpoint_t;
typedef struct { bool wifi_connected; } d1l_connectivity_status_t;
static d1l_observer_endpoint_t s_endpoints[3];
static int s_lock, s_client_lock=1, s_wifi_control_lock=1;
static bool cancelled, wifi_connected=true, enabled=true;
static bool client_locked, control_locked, mutex_failure, destroy_failure, quiesce_failure;
static bool cancel_while_taking_lock;
static unsigned destroyed, stop_calls;
static bool take_client_lock(void) {
    if (mutex_failure) return false;
    assert(!client_locked); client_locked=true;
    if (cancel_while_taking_lock) cancelled=true;
    return true;
}
static void give_client_lock(void) { assert(client_locked); client_locked=false; }
static bool take_wifi_control(void) { assert(!control_locked); control_locked=true; return true; }
static void give_wifi_control(void) { assert(control_locked); control_locked=false; }
static bool observer_enabled(void) { return enabled; }
static void d1l_connectivity_status(d1l_connectivity_status_t *out) { out->wifi_connected=wifi_connected && !cancelled; }
static bool d1l_connectivity_network_cancel_requested(void) { return cancelled; }
static esp_err_t wifi_network_quiesce_begin(uint32_t ms) {
    assert(control_locked && ms==20000);
    if (quiesce_failure) return ESP_ERR_TIMEOUT;
    cancelled=true; return ESP_OK;
}
static void wifi_network_quiesce_end(void) { cancelled=false; }
static esp_err_t esp_mqtt_client_stop(void *client) {
    assert(client && client_locked && wifi_connected); stop_calls++; return ESP_OK;
}
static esp_err_t esp_mqtt_client_destroy(void *client) {
    assert(client && client_locked && wifi_connected);
    if (destroy_failure && client==(void *)1) return ESP_FAIL;
    destroyed++; return ESP_OK;
}
static void secure_zero(void *ptr, size_t size) { memset(ptr,0,size); }
'''
    for name in ["observer_network_continue", "take_network_client_lock", "stop_endpoint_locked", "d1l_observer_prepare_network_shutdown"]:
        code += "\n" + function(observer, name)
    code += "\n" + function(connectivity, "wifi_shutdown_begin")
    code += r'''
static void populate(void) {
    for (unsigned i=0; i<3; ++i) {
        s_endpoints[i]=(d1l_observer_endpoint_t){.client=(void *)(uintptr_t)(i+1),.connected=true};
        strcpy(s_endpoints[i].password,"fixture-only");
    }
    destroyed=stop_calls=0;
}
static void wifi_driver_teardown(void) {
    assert(cancelled && control_locked && !client_locked && destroyed==3);
    for (unsigned i=0; i<3; ++i) assert(s_endpoints[i].client==NULL);
    wifi_connected=false;
}
int main(void) {
    bool control=false;
    populate();
    assert(wifi_shutdown_begin(&control)==ESP_OK && control);
    assert(destroyed==3 && stop_calls==3 && cancelled);
    for (unsigned i=0; i<3; ++i) assert(!s_endpoints[i].connected && s_endpoints[i].password[0]=='\0');
    assert(!take_network_client_lock() && !client_locked);
    wifi_driver_teardown();
    wifi_network_quiesce_end(); give_wifi_control();
    /* Stale worker snapshots must not restart clients after the barrier ends. */
    assert(!take_network_client_lock() && !client_locked);
    wifi_connected=true;
    assert(take_network_client_lock()); give_client_lock();
    enabled=false;
    assert(!take_network_client_lock() && !client_locked);
    enabled=true;
    cancel_while_taking_lock=true;
    assert(!take_network_client_lock() && !client_locked);
    cancel_while_taking_lock=false; cancelled=false;
    /* Failure leaves the live driver intact and all locks unwind. */
    populate(); destroy_failure=true;
    assert(wifi_shutdown_begin(&control)==ESP_FAIL);
    assert(!control && !control_locked && !cancelled && !client_locked && wifi_connected);
    assert(s_endpoints[0].client==(void *)1 && destroyed==2);
    destroy_failure=false;
    assert(wifi_shutdown_begin(&control)==ESP_OK && control);
    assert(destroyed==3); wifi_driver_teardown();
    wifi_network_quiesce_end(); give_wifi_control();
    wifi_connected=true; populate(); mutex_failure=true;
    assert(wifi_shutdown_begin(&control)==ESP_ERR_TIMEOUT);
    assert(!control_locked && !cancelled && destroyed==0);
    mutex_failure=false; quiesce_failure=true;
    assert(wifi_shutdown_begin(&control)==ESP_ERR_TIMEOUT);
    assert(!control_locked && !cancelled && destroyed==0);
    quiesce_failure=false; s_client_lock=0; memset(s_endpoints,0,sizeof(s_endpoints));
    assert(d1l_observer_prepare_network_shutdown()==ESP_OK);
    return 0;
}
'''
    path = tmp_path / "shutdown.c"
    path.write_text(code, encoding="utf-8")
    exe = tmp_path / "shutdown"
    subprocess.run([shutil.which("gcc") or "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "tests/native/stubs"), str(path), "-o", str(exe)],
                   check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    assert "take_network_client_lock()" in function(observer, "start_endpoint")
    publish = observer.split("static int publish_to_endpoint", 1)[1].split("static bool observer_enabled", 1)[0]
    assert "!take_network_client_lock()" in publish
