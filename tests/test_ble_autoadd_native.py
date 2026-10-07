"""Run the production auto-add command handlers with real phone frame bytes."""
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_phone_autoadd_frames_preserve_policy_and_report_failed_saves(tmp_path):
    protocol = (ROOT / "main/comms/ble_companion_protocol.c").read_text(encoding="utf-8")
    handlers = ""
    for start, end in [
        ("set_other_params_command", "set_tuning_params_command"),
        ("set_autoadd_config_command", "queue_self_advert_command"),
        ("build_autoadd_config", "dispatch_command"),
    ]:
        handlers += "static void " + start + protocol.split("static void " + start, 1)[1].split("static void " + end, 1)[0]
    code = r'''
#include <assert.h>
#include <string.h>
#include "app/settings_model.h"
#include "mesh/contact_policy.h"
#define ERR_CODE_ILLEGAL_ARG 6U
#define RESP_CODE_DISABLED 18U
#define RESP_CODE_AUTOADD_CONFIG 25U
static d1l_contact_policy_t current = {.roles=30, .max_hops=3};
static esp_err_t save_error, last_result;
static unsigned saves, last_error, simple;
static uint8_t response[3];
d1l_contact_policy_t d1l_contact_policy_get(void) { return current; }
esp_err_t d1l_contact_policy_save(d1l_contact_policy_t policy) {
    ++saves; if(save_error==ESP_OK) current=policy; return save_error;
}
esp_err_t d1l_settings_public_snapshot(d1l_settings_t *out) {
    *out=(d1l_settings_t){0}; return ESP_OK;
}
static void set_error_response(uint8_t error) { last_error=error; }
static void set_result_response(esp_err_t result) { last_result=result; }
static void set_simple_response(uint8_t code) { simple=code; }
static bool set_pending(const uint8_t *data, size_t length) {
    assert(length==3); memcpy(response,data,3); return true;
}
''' + handlers + r'''
int main(void) {
    uint8_t frame[6]={58,2,1};
    set_autoadd_config_command(frame,2);
    assert(saves==1 && current.roles==2 && current.max_hops==3);
    set_autoadd_config_command(frame,3);
    assert(saves==2 && current.max_hops==1);
    build_autoadd_config(); assert(response[0]==25 && response[1]==2 && response[2]==1);
    frame[1]=31; set_autoadd_config_command(frame,3);
    assert(saves==2 && last_error==ERR_CODE_ILLEGAL_ARG);
    frame[1]=2; frame[2]=65; set_autoadd_config_command(frame,3);
    assert(saves==2);
    frame[2]=0; set_autoadd_config_command(frame,4); assert(saves==2);
    save_error=ESP_FAIL; frame[1]=16; set_autoadd_config_command(frame,3);
    assert(last_result==ESP_FAIL && current.roles==2);
    save_error=ESP_OK; frame[1]=1; frame[2]=0;
    set_other_params_command(frame,2); assert(current.manual);
    frame[1]=0; set_other_params_command(frame,2); assert(!current.manual);
    unsigned before=saves; frame[2]=1;
    set_other_params_command(frame,3); assert(saves==before && simple==RESP_CODE_DISABLED);
    return 0;
}
'''
    source = tmp_path / "autoadd.c"
    source.write_text(code, encoding="utf-8")
    exe = tmp_path / ("autoadd.exe" if os.name == "nt" else "autoadd")
    subprocess.run([shutil.which("gcc") or "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "tests/native/stubs"), "-I", str(ROOT / "main"),
                    str(source), "-o", str(exe)], check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True)
