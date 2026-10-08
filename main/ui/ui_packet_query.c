#include "ui_packet_query.h"

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

typedef struct {
    size_t limit;
    size_t skip;
    char direction[4];
    char kind[16];
    char search[D1L_PACKET_LOG_QUERY_TEXT_LEN];
} packet_request_t;

typedef struct {
    uint32_t request_generation;
    uint32_t query_generation;
    uint32_t sd_generation;
    bool interrupted;
} packet_scan_t;

static portMUX_TYPE s_query_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_query_task;
static packet_request_t s_request;
static uint64_t s_revision;
static uint32_t s_generation;
static bool s_valid;
static bool s_pending;
static bool s_ready;
static d1l_ui_packet_query_result_t s_result EXT_RAM_BSS_ATTR;
static d1l_packet_log_entry_t
    s_query_rows[D1L_UI_PACKETS_INITIAL_ROWS + 1U] EXT_RAM_BSS_ATTR;

static void next_generation_locked(void)
{
    if (++s_generation == 0U) s_generation = 1U;
}

static bool scan_current(void *context)
{
    packet_scan_t *scan = context;
    portENTER_CRITICAL(&s_query_mux);
    const bool current = s_valid && s_generation == scan->request_generation;
    portEXIT_CRITICAL(&s_query_mux);
    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    const bool valid = current && stats.loaded && !stats.clear_in_progress &&
        stats.query_generation == scan->query_generation &&
        stats.sd_backend_generation == scan->sd_generation;
    scan->interrupted = scan->interrupted || !valid;
    return valid;
}

static void process_request(void)
{
    packet_request_t request;
    packet_scan_t scan = {0};
    portENTER_CRITICAL(&s_query_mux);
    const bool pending = s_valid && s_pending;
    request = s_request;
    scan.request_generation = s_generation;
    portEXIT_CRITICAL(&s_query_mux);
    if (!pending) return;

    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    scan.query_generation = stats.query_generation;
    scan.sd_generation = stats.sd_backend_generation;
    bool sd_used = false;
    esp_err_t error = ESP_OK;
    /* One extra matching row proves that Older is available. Computing an
     * exact filtered total would scan the entire 4096-record archive. */
    const size_t count = d1l_packet_log_query_page_cancellable(
        s_query_rows, request.limit + 1U, request.skip, request.direction,
        request.kind, request.search, NULL, &sd_used, &error, scan_current, &scan);
    const bool valid = scan_current(&scan) && !scan.interrupted &&
        count <= request.limit + 1U;

    portENTER_CRITICAL(&s_query_mux);
    if (s_valid && s_generation == scan.request_generation) {
        memset(&s_result, 0, sizeof(s_result));
        s_result.query_generation = scan.query_generation;
        s_result.sd_generation = scan.sd_generation;
        s_result.error = valid ? error : ESP_ERR_INVALID_STATE;
        if (s_result.error == ESP_OK) {
            s_result.has_more = count > request.limit;
            s_result.count = s_result.has_more ? request.limit : count;
            s_result.sd_used = sd_used;
            /* Store queries return chronological rows; the lookahead is the
             * oldest one, not the newest displayed packet. */
            memcpy(s_result.rows, s_query_rows + (s_result.has_more ? 1U : 0U),
                   s_result.count * sizeof(s_result.rows[0]));
        }
        s_pending = false;
        s_ready = true;
    }
    portEXIT_CRITICAL(&s_query_mux);
}

static void query_task(void *argument)
{
    (void)argument;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        process_request();
    }
}

esp_err_t d1l_ui_packet_query_submit(const d1l_ui_packets_query_request_t *request,
                                    uint64_t revision)
{
    if (!request || request->row_limit == 0U ||
        request->row_limit > D1L_UI_PACKETS_INITIAL_ROWS ||
        request->skip_newest > D1L_PACKET_LOG_SD_CAPACITY) return ESP_ERR_INVALID_ARG;
    packet_request_t next = {.limit = request->row_limit, .skip = request->skip_newest};
    snprintf(next.direction, sizeof(next.direction), "%s", request->direction ? request->direction : "any");
    snprintf(next.kind, sizeof(next.kind), "%s", request->kind ? request->kind : "any");
    snprintf(next.search, sizeof(next.search), "%s", request->search_text ? request->search_text : "");
    if (!s_query_task && xTaskCreatePinnedToCoreWithCaps(
            query_task, "d1l_pkt_query", 8192U, NULL, 2, &s_query_task,
            0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_query_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_query_mux);
    const bool same = s_valid && s_request.limit == next.limit &&
        s_request.skip == next.skip && strcmp(s_request.direction, next.direction) == 0 &&
        strcmp(s_request.kind, next.kind) == 0 && strcmp(s_request.search, next.search) == 0;
    const bool schedule = !same || (!s_pending && !s_ready && revision != s_revision);
    if (schedule) {
        next_generation_locked();
        s_request = next;
        s_revision = revision;
        s_valid = s_pending = true;
        s_ready = false;
    }
    portEXIT_CRITICAL(&s_query_mux);
    if (schedule) xTaskNotifyGive(s_query_task);
    return ESP_OK;
}

bool d1l_ui_packet_query_pending(void)
{
    portENTER_CRITICAL(&s_query_mux);
    const bool value = s_pending;
    portEXIT_CRITICAL(&s_query_mux);
    return value;
}

bool d1l_ui_packet_query_ready(void)
{
    portENTER_CRITICAL(&s_query_mux);
    const bool value = s_ready;
    portEXIT_CRITICAL(&s_query_mux);
    return value;
}

bool d1l_ui_packet_query_take(d1l_ui_packet_query_result_t *result)
{
    if (!result) return false;
    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    portENTER_CRITICAL(&s_query_mux);
    const bool ready = s_ready;
    if (ready) {
        *result = s_result;
        s_ready = false;
    }
    portEXIT_CRITICAL(&s_query_mux);
    if (ready && (result->query_generation != stats.query_generation ||
                  result->sd_generation != stats.sd_backend_generation ||
                  stats.clear_in_progress)) {
        result->error = ESP_ERR_INVALID_STATE;
        result->count = 0U;
        result->has_more = false;
    }
    return ready;
}

void d1l_ui_packet_query_cancel(void)
{
    portENTER_CRITICAL(&s_query_mux);
    next_generation_locked();
    s_valid = s_pending = s_ready = false;
    memset(&s_result, 0, sizeof(s_result));
    portEXIT_CRITICAL(&s_query_mux);
}
