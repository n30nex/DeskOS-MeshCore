"""Run the patched board's failure paths without a physical RF transmission."""
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"(?:static\s+)?(?:void|bool|uint8_t|uint32_t|esp_err_t)\s+" + name + r"\s*\([^)]*\)\s*\{", source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


def patched_driver(tmp_path):
    bsp = ROOT / "third_party/sensecap_indicator_esp32"
    for name in ["radio.c", "radio.h", "sx126x_sensecap_board.c", "bsp_sx126x.h"]:
        relative = "components/lora/" + name
        path = tmp_path / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(subprocess.check_output(["git", "show", "HEAD:" + relative], cwd=bsp))
    for patch in ["sensecap_indicator_idf55_compat.patch", "sensecap_indicator_tx_origin.patch", "sensecap_indicator_airtime.patch"]:
        subprocess.run(["git", "apply", "--unidiff-zero", "--ignore-space-change", "--include=components/lora/*",
                        str(ROOT / "patches" / patch)], cwd=tmp_path, check=True)
    return tmp_path / "components/lora"


def compile_run(tmp_path, code):
    source, binary = tmp_path / "check.c", tmp_path / "check"
    source.write_text(code)
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_busy_timeout_blocks_spi_and_recovers_under_exclusive_irq_claim(tmp_path):
    driver = patched_driver(tmp_path)
    board, radio = (driver / "sx126x_sensecap_board.c").read_text(), (driver / "radio.c").read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int esp_err_t;
typedef uint8_t RadioCommands_t;
#define ESP_OK 0
#define EXPANDER_IO_RADIO_BUSY 2
#define EXPANDER_IO_RADIO_NSS 0
#define EXPANDER_IO_RADIO_RST 1
#define RADIO_GET_STATUS 0xc0
#define RADIO_SET_SLEEP 0x84
#define MODE_SLEEP 0
#define MODE_STDBY_RC 1
#define portMAX_DELAY -1
#define portTICK_PERIOD_MS 10
typedef struct {size_t length; const void *tx_buffer; void *rx_buffer;} spi_transaction_t;
static int SpiHandle=1, radio_mutex=1, pins_mode, reads, spi_calls, chip_selects, locks;
static int busy_reads, hardware_resets, timer_stops, spi_error;
static int64_t now_us;
static volatile uint32_t radio_bus_fault, radio_bus_fault_count;
static int64_t esp_timer_get_time(void) {return now_us;}
static void vTaskDelay(int ticks) {assert(ticks>0); now_us+=(int64_t)ticks*10000;}
static void xSemaphoreTake(int mutex,int timeout) {(void)mutex;(void)timeout;assert(!locks);locks++;}
static void xSemaphoreGive(int mutex) {(void)mutex;assert(locks==1);locks--;}
static int read_pins(uint16_t *pins) {
    reads++;
    if(pins_mode==3) return -1;
    bool busy=pins_mode==2 || busy_reads>0;
    if(busy_reads>0) busy_reads--;
    *pins=busy ? 1U<<EXPANDER_IO_RADIO_BUSY : 0U;
    return ESP_OK;
}
static void set_pin(int pin,int value) {
    if(pin==EXPANDER_IO_RADIO_NSS && value==0) chip_selects++;
    if(pin==EXPANDER_IO_RADIO_RST && value==0) hardware_resets++;
}
static struct {int (*read_input_pins16)(uint16_t*);void (*set_level)(int,int);} expander={read_pins,set_pin};
static void *unused_pointer;
#define indicator_io_expander (&expander)
static int spi_device_transmit(int handle,spi_transaction_t *transaction) {
    (void)handle;spi_calls++;
    if(transaction->rx_buffer) memset(transaction->rx_buffer,0x54,transaction->length/8);
    return spi_error;
}
static void SX126xSetOperatingMode(int mode) {(void)mode;}
static void SX126xWaitOnBusy(void);
static void SX126xCheckDeviceReady(void) {SX126xWaitOnBusy();}
'''
    # Use the production functions, including the checks before chip select.
    for name in ["bsp_sx126x_fault_get", "bsp_sx126x_fault_count_get", "latch_radio_bus_fault",
                 "spi_write_byte", "spi_read_byte", "spi_transfer", "SX126xReset", "SX126xWaitOnBusy",
                 "SX126xWriteCommand", "SX126xReadCommand"]:
        code += function(board, name)
    code += r'''
static volatile uint32_t RadioIrqRecoveryClaim, RadioTxOrigin;
static uint32_t RadioTxTimeoutOrigin,RadioIrqTxOrigin;
static bool IrqFired;
static int TxTimeoutTimer,RxTimeoutTimer;
static void TimerStop(int *timer) {(void)timer;timer_stops++;}
static void RadioInitHardware(void) {SX126xReset();SX126xWaitOnBusy();}
'''
    for name in ["RadioTryClaimIrqRecovery", "RadioReleaseIrqRecovery", "RadioRecoverHardware"]:
        code += function(radio, name)
    code += r'''
int main(void) {
    (void)unused_pointer;
    uint8_t bytes[2]={7,8};
    busy_reads=3;
    SX126xWriteCommand(RADIO_GET_STATUS,bytes,sizeof(bytes));
    assert(now_us==30000 && bsp_sx126x_fault_get()==0 && spi_calls==2);
    pins_mode=2;spi_calls=chip_selects=0;
    int64_t started=now_us;
    SX126xWriteCommand(RADIO_GET_STATUS,bytes,sizeof(bytes));
    assert(now_us-started==1000000 && bsp_sx126x_fault_get()==1);
    assert(bsp_sx126x_fault_count_get()==1 && spi_calls==0 && chip_selects==0);
    int prior_reads=reads;
    assert(SX126xReadCommand(RADIO_GET_STATUS,bytes,sizeof(bytes))==0);
    assert(bytes[0]==0 && bytes[1]==0 && reads==prior_reads && spi_calls==0);
    RadioTxOrigin=17;RadioIrqRecoveryClaim=1;
    assert(!RadioRecoverHardware() && hardware_resets==0 && timer_stops==0 && RadioTxOrigin==17);
    RadioIrqRecoveryClaim=0;pins_mode=0;
    assert(RadioRecoverHardware());
    assert(RadioTxOrigin==0 && RadioIrqRecoveryClaim==0 && hardware_resets==1 && timer_stops==2);
    assert(bsp_sx126x_fault_get()==0 && bsp_sx126x_fault_count_get()==1);
    assert(SX126xReadCommand(RADIO_GET_STATUS,bytes,sizeof(bytes))==0x54);
    assert(spi_calls==3 && bytes[0]==0x54 && bytes[1]==0x54);
    pins_mode=3;spi_calls=0;
    SX126xWriteCommand(RADIO_GET_STATUS,bytes,sizeof(bytes));
    assert(bsp_sx126x_fault_get()==2 && spi_calls==0 && bsp_sx126x_fault_count_get()==2);
    assert(!RadioRecoverHardware() && RadioIrqRecoveryClaim==0 && bsp_sx126x_fault_get()==2);
    pins_mode=0;assert(RadioRecoverHardware());
    radio_bus_fault_count=UINT32_MAX;pins_mode=2;
    SX126xWaitOnBusy();assert(bsp_sx126x_fault_count_get()==UINT32_MAX);
    pins_mode=0;assert(RadioRecoverHardware());
    spi_error=1;spi_calls=0;
    SX126xWriteCommand(RADIO_GET_STATUS,bytes,sizeof(bytes));
    assert(bsp_sx126x_fault_get()==3 && spi_calls==1);
    return 0;
}
'''
    compile_run(tmp_path, code)


def test_companion_reports_radio_rejection_and_preserves_channel_frame(tmp_path):
    protocol = (ROOT / "main/comms/ble_companion_protocol.c").read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_NOT_FOUND 4
#define ESP_ERR_INVALID_ARG 5
#define ESP_ERR_INVALID_SIZE 6
#define ESP_ERR_NOT_SUPPORTED 7
#define ESP_ERR_NOT_FINISHED 8
#define ERR_CODE_UNSUPPORTED_CMD 1
#define ERR_CODE_NOT_FOUND 2
#define ERR_CODE_TABLE_FULL 3
#define ERR_CODE_BAD_STATE 4
#define ERR_CODE_FILE_IO_ERROR 5
#define ERR_CODE_ILLEGAL_ARG 6
#define RESP_CODE_OK 0
#define D1L_MESSAGE_TEXT_LEN 139
typedef struct {uint64_t channel_id;} d1l_channel_info_t;
static int result,reply,confirmed_calls,last_length;
static uint8_t used_index;
static char sent_text[D1L_MESSAGE_TEXT_LEN];
static void set_simple_response(uint8_t code) {reply=code;}
static void set_error_response(uint8_t code) {reply=100+code;}
static void note_error(int error,bool unsupported) {(void)error;(void)unsupported;}
static void note_text_command(uint8_t type,size_t length) {assert(type==0);last_length=(int)length;}
static int channel_at_index(uint8_t index,d1l_channel_info_t *channel) {used_index=index;channel->channel_id=99;return ESP_OK;}
static int d1l_app_model_send_channel_text_confirmed(uint64_t id,const char *text) {
    assert(id==99);confirmed_calls++;strcpy(sent_text,text);return result;
}
'''
    code += function(protocol, "protocol_error") + function(protocol, "set_result_response") + function(protocol, "send_channel_command")
    code += r'''
int main(void) {
    uint8_t frame[]={3,0,2,1,2,3,4,'t','e','s','t'};
    assert(protocol_error(ESP_ERR_NOT_FINISHED)==ERR_CODE_BAD_STATE);
    result=ESP_ERR_INVALID_STATE;
    send_channel_command(frame,sizeof(frame));
    assert(reply==104 && confirmed_calls==1 && used_index==2 && last_length==4);
    assert(strcmp(sent_text,"test")==0);
    result=ESP_ERR_TIMEOUT;send_channel_command(frame,sizeof(frame));assert(reply==103);
    result=ESP_OK;send_channel_command(frame,sizeof(frame));assert(reply==0 && confirmed_calls==3);
    send_channel_command(frame,7);assert(reply==106 && confirmed_calls==3);
    return 0;
}
'''
    compile_run(tmp_path, code)


def test_owner_recovery_closes_one_tx_and_restores_profile_before_ready(tmp_path):
    service = (ROOT / "main/mesh/meshcore_service.c").read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define D1L_MESHCORE_SERVICE_RADIO_ERROR 4
#define D1L_MESHCORE_SERVICE_EVENT_TX_TIMEOUT 7
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
typedef struct {uint64_t operation_id;} d1l_mesh_tx_operation_identity_t;
typedef struct {int type;uint64_t monotonic_us;d1l_mesh_tx_operation_identity_t tx_operation;} d1l_meshcore_service_cmd_t;
static bool s_radio_started,s_radio_recovery_pending,s_radio_profile_applied;
static uint32_t s_radio_recovery_attempts,s_radio_recovery_successes;
static uint64_t s_radio_recovery_not_before_us,now_us;
static d1l_mesh_tx_operation_identity_t s_active_radio_tx;
static struct {bool radio_ready;int state;} s_status;
static uint32_t fault,reset_calls,timeout_calls,profile_calls;
static uint64_t closed_operation;
static bool driver_ok,profile_ok;
static uint32_t bsp_sx126x_fault_get(void) {return fault;}
static int64_t esp_timer_get_time(void) {return (int64_t)now_us;}
static uint32_t d1l_mesh_runtime_counter_increment_saturating(uint32_t *value) {if(*value!=UINT32_MAX)++*value;return *value;}
static bool d1l_mesh_tx_operation_identity_valid(const d1l_mesh_tx_operation_identity_t *op) {return op->operation_id!=0;}
static void meshcore_service_handle_radio_tx_timeout(const d1l_meshcore_service_cmd_t *event) {
    assert(event->type==D1L_MESHCORE_SERVICE_EVENT_TX_TIMEOUT);
    assert(!s_status.radio_ready && s_radio_recovery_pending);
    timeout_calls++;closed_operation=event->tx_operation.operation_id;
    s_active_radio_tx.operation_id=0;
}
static bool recover(void) {reset_calls++;if(driver_ok)fault=0;return driver_ok;}
static const struct {bool (*RecoverHardware)(void);} Radio={recover};
static esp_err_t meshcore_service_handle_start_rx(void) {
    assert(fault==0 && !s_radio_recovery_pending);profile_calls++;
    if(!profile_ok)return 1;
    s_radio_profile_applied=true;s_status.radio_ready=true;return ESP_OK;
}
'''
    code += function(service, "meshcore_service_recover_radio_bus")
    code += r'''
int main(void) {
    fault=1;meshcore_service_recover_radio_bus();assert(reset_calls==0);
    s_radio_started=true;fault=0;meshcore_service_recover_radio_bus();assert(reset_calls==0);
    fault=1;s_active_radio_tx.operation_id=91;s_status.radio_ready=true;now_us=1000000;
    meshcore_service_recover_radio_bus();
    assert(timeout_calls==1 && closed_operation==91 && reset_calls==1 && s_radio_recovery_pending);
    assert(!s_status.radio_ready && s_status.state==D1L_MESHCORE_SERVICE_RADIO_ERROR);
    now_us=2000000;meshcore_service_recover_radio_bus();assert(reset_calls==1 && timeout_calls==1);
    driver_ok=true;now_us=3000000;meshcore_service_recover_radio_bus();
    assert(reset_calls==2 && profile_calls==1 && s_radio_recovery_pending && !s_status.radio_ready);
    profile_ok=true;now_us=5000000;meshcore_service_recover_radio_bus();
    assert(reset_calls==3 && profile_calls==2 && !s_radio_recovery_pending && s_status.radio_ready);
    assert(s_radio_profile_applied && s_radio_recovery_successes==1 && s_radio_recovery_attempts==3);
    now_us=6000000;meshcore_service_recover_radio_bus();assert(reset_calls==3 && timeout_calls==1);
    return 0;
}
'''
    compile_run(tmp_path, code)


def test_confirmed_channel_caller_never_owns_shared_send_cleanup(tmp_path):
    service = (ROOT / "main/mesh/meshcore_service.c").read_text()
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_TIMEOUT 2
#define D1L_MESHCORE_SERVICE_CMD_SEND_CHANNEL 9
#define D1L_MESHCORE_CHANNEL_COMMAND_TIMEOUT_MS 5000U
typedef struct {int type;uint64_t channel_id;char dm_text[139];} d1l_meshcore_service_cmd_t;
static unsigned shared_send=41,submit_count,wipe_count;
static int result=ESP_ERR_TIMEOUT;
static d1l_meshcore_service_cmd_t submitted;
static esp_err_t validate_user_text(const char *text) {return text && *text ? ESP_OK : ESP_ERR_INVALID_ARG;}
static esp_err_t meshcore_service_send_command(d1l_meshcore_service_cmd_t *cmd,uint32_t timeout) {
    assert(timeout==5000 && cmd->type==D1L_MESHCORE_SERVICE_CMD_SEND_CHANNEL);
    submitted=*cmd;submit_count++;
    /* The owner can finish A and admit B before the caller resumes. */
    shared_send=99;return result;
}
static void meshcore_service_command_wipe(d1l_meshcore_service_cmd_t *cmd) {memset(cmd,0,sizeof(*cmd));wipe_count++;}
'''
    code += function(service, "d1l_meshcore_service_send_channel_confirmed")
    code += r'''
int main(void) {
    assert(d1l_meshcore_service_send_channel_confirmed(7,"test")==ESP_ERR_TIMEOUT);
    assert(submit_count==1 && wipe_count==1 && shared_send==99);
    assert(submitted.channel_id==7 && strcmp(submitted.dm_text,"test")==0);
    result=ESP_OK;
    assert(d1l_meshcore_service_send_channel_confirmed(8,"next")==ESP_OK);
    assert(shared_send==99 && submitted.channel_id==8 && submit_count==2);
    assert(d1l_meshcore_service_send_channel_confirmed(0,"test")==ESP_ERR_INVALID_ARG);
    assert(d1l_meshcore_service_send_channel_confirmed(8,"")==ESP_ERR_INVALID_ARG);
    assert(submit_count==2);
    return 0;
}
'''
    compile_run(tmp_path, code)
