import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def test_meshcore_ack_dispatch_native_vectors(tmp_path: Path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for native MeshCore ACK dispatch tests")

    binary = tmp_path / (
        "meshcore_ack_dispatch_test.exe" if os.name == "nt" else "meshcore_ack_dispatch_test"
    )
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main/mesh/meshcore_wire.c"),
        str(ROOT / "tests/native/meshcore_ack_dispatch_test.c"),
        "-o",
        str(binary),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    result = subprocess.run(
        [str(binary)], cwd=ROOT, check=True, capture_output=True, text=True
    )
    assert result.stdout.strip() == "meshcore ACK dispatch vectors passed"


def test_production_ack_admission_waits_for_busy_radio(tmp_path: Path):
    service = (ROOT / "main/mesh/meshcore_service.c").read_text(encoding="utf-8")
    marker = "static esp_err_t meshcore_service_send_ack_async("
    handler = marker + service.rsplit(marker, 1)[1].split(
        "void d1l_meshcore_service_init(void)", 1
    )[0]
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT };
enum { D1L_MESHCORE_SERVICE_CMD_SEND_RAW = 1,
       D1L_MESH_TX_OPERATION_ACK_RESPONSE = 2, pdTRUE = 1 };
#define D1L_MESHCORE_DM_ACK_DEDUPE_DIGEST_BYTES 32
typedef struct { int unused; } d1l_contact_entry_t;
typedef struct { uint32_t delay_ms; } d1l_meshcore_ack_dispatch_plan_t;
typedef struct {
    int type, requested_tx_kind;
    uint8_t raw_len, raw[255];
    uint16_t delay_ms;
    bool ack_response;
} d1l_meshcore_service_cmd_t;
static bool s_tx_busy, queue_full;
static struct { bool active; } s_pending_ack_tx;
static struct {
    unsigned ack_tx_failed, ack_tx_last_hash, ack_tx_queued;
    esp_err_t ack_tx_last_error;
} s_status;
static unsigned s_runtime_priority_queue_high_water;
static int s_priority_command_queue, queued, wakes, completions;
static esp_err_t completion_error;
static d1l_meshcore_service_cmd_t queued_command;
static esp_err_t meshcore_service_start_task(void) { return ESP_OK; }
static void complete_unqueued_ack_reservation(
    uint32_t row, const uint8_t *digest, esp_err_t error) {
    assert(row == 7 && digest); ++completions; completion_error = error;
}
static void remember_pending_ack_tx(
    const d1l_contact_entry_t *contact, uint32_t hash, uint32_t row,
    const uint8_t *digest, const d1l_meshcore_ack_dispatch_plan_t *plan,
    const uint8_t *raw, uint8_t length) {
    assert(contact && hash == 42 && row == 7 && digest && plan && raw && length);
    s_pending_ack_tx.active = true;
}
static void complete_pending_ack_tx(bool sent, esp_err_t error) {
    assert(!sent); s_pending_ack_tx.active = false;
    ++completions; completion_error = error;
}
static int xQueueSendToFront(int queue, const void *command, int wait) {
    (void)queue; assert(wait == 0);
    if (queue_full) return 0;
    ++queued; queued_command = *(const d1l_meshcore_service_cmd_t *)command;
    return pdTRUE;
}
static void runtime_note_command_saturation(bool priority) { assert(priority); }
static void runtime_note_queue_depth(int queue, unsigned *high_water) {
    (void)queue; *high_water = 1;
}
static void meshcore_service_wake(void) { ++wakes; }
''' + handler + r'''
int main(void) {
    d1l_contact_entry_t contact = {0};
    uint8_t digest[32] = {1}, raw[] = {2, 3, 4};
    d1l_meshcore_ack_dispatch_plan_t plan = {.delay_ms = 12};
    s_tx_busy = true;
    assert(meshcore_service_send_ack_async(
        &contact, 42, 7, digest, &plan, raw, sizeof(raw)) == ESP_OK);
    assert(s_tx_busy && s_pending_ack_tx.active && queued == 1 && wakes == 1);
    assert(s_status.ack_tx_queued == 1 && s_status.ack_tx_failed == 0);
    assert(queued_command.ack_response && queued_command.delay_ms == 12);
    assert(queued_command.type == D1L_MESHCORE_SERVICE_CMD_SEND_RAW);
    assert(queued_command.requested_tx_kind == D1L_MESH_TX_OPERATION_ACK_RESPONSE);
    assert(queued_command.raw_len == sizeof(raw));
    assert(memcmp(queued_command.raw, raw, sizeof(raw)) == 0);
    /* A second reservation cannot overwrite the queued ACK. */
    assert(meshcore_service_send_ack_async(
        &contact, 42, 7, digest, &plan, raw, sizeof(raw)) == ESP_ERR_INVALID_STATE);
    assert(queued == 1 && s_pending_ack_tx.active && completions == 1);
    assert(completion_error == ESP_ERR_INVALID_STATE);
    s_pending_ack_tx.active = false;
    queue_full = true;
    assert(meshcore_service_send_ack_async(
        &contact, 42, 7, digest, &plan, raw, sizeof(raw)) == ESP_ERR_TIMEOUT);
    assert(!s_pending_ack_tx.active && queued == 1 && completions == 2);
    assert(completion_error == ESP_ERR_TIMEOUT);
    return 0;
}
'''
    source = tmp_path / "ack_admission.c"
    source.write_text(code, encoding="utf-8")
    binary = tmp_path / ("ack_admission.exe" if os.name == "nt" else "ack_admission")
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required for production ACK admission tests"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(binary)], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
