#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Drive the production worker step deterministically at its I/O boundaries. */
#include "../../main/ui/ui_packet_query.c"

static unsigned s_calls;
static unsigned s_notified;
static bool s_create_fails;
static bool s_replace_during_read;
static bool s_clear_during_read;
static d1l_packet_log_stats_t s_stats = {
    .loaded = true, .query_generation = 1U, .sd_backend_generation = 1U,
};

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t function,
    const char *name, uint32_t stack, void *argument, UBaseType_t priority,
    TaskHandle_t *handle, BaseType_t core, UBaseType_t caps)
{
    (void)name; (void)argument;
    assert(function && stack == 8192U && priority == 2U && core == 0);
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_create_fails) return 0;
    *handle = (void *)1;
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task); ++s_notified; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    (void)clear; (void)wait; assert(false); return 0;
}
d1l_packet_log_stats_t d1l_packet_log_stats(void) { return s_stats; }
size_t d1l_packet_log_query_page_cancellable(
    d1l_packet_log_entry_t *rows, size_t limit, size_t skip,
    const char *direction, const char *kind, const char *search,
    size_t *total, bool *sd, esp_err_t *error, d1l_packet_query_continue_fn_t keep_going, void *context)
{
    (void)direction; (void)kind;
    ++s_calls;
    *error = ESP_OK;
    assert(total == NULL && limit == 13U && keep_going(context));
    assert(strcmp(search, "saved") == 0 || strcmp(search, "new") == 0);
    if (s_replace_during_read) {
        s_replace_during_read = false;
        const d1l_ui_packets_query_request_t next = {12U, 12U, "any", "text", "new"};
        assert(d1l_ui_packet_query_submit(&next, 2U) == ESP_OK);
        assert(!keep_going(context));
        return 1U;
    }
    if (s_clear_during_read) {
        s_clear_during_read = false;
        ++s_stats.query_generation;
        assert(!keep_going(context));
        return 1U;
    }
    for (size_t i = 0; i < limit; ++i) rows[i].seq = (uint32_t)(100U + skip + i);
    *sd = true;
    return limit;
}

int main(void)
{
    char text[] = "saved";
    d1l_ui_packets_query_request_t request = {12U, 0U, "any", "text", text};
    d1l_ui_packet_query_result_t result;
    s_create_fails = true;
    assert(d1l_ui_packet_query_submit(&request, 1U) == ESP_ERR_NO_MEM);
    assert(!d1l_ui_packet_query_pending() && s_calls == 0U);
    s_create_fails = false;
    assert(d1l_ui_packet_query_submit(&request, 1U) == ESP_OK);
    assert(d1l_ui_packet_query_pending() && s_calls == 0U && s_notified == 1U);
    assert(d1l_ui_packet_query_submit(&request, 2U) == ESP_OK);
    assert(s_notified == 1U); /* Live traffic does not starve an in-flight scan. */
    strcpy(text, "xxxxx"); /* The worker owns a copy, not a textarea pointer. */
    process_request();
    assert(d1l_ui_packet_query_ready() && d1l_ui_packet_query_take(&result));
    assert(result.error == ESP_OK && result.count == 12U && result.has_more);
    assert(result.rows[0].seq == 101U && result.rows[11].seq == 112U);

    request.search_text = "saved";
    s_replace_during_read = true;
    assert(d1l_ui_packet_query_submit(&request, 3U) == ESP_OK);
    process_request();
    assert(!d1l_ui_packet_query_ready() && d1l_ui_packet_query_pending());
    process_request();
    assert(d1l_ui_packet_query_take(&result) && result.rows[0].seq == 113U);

    s_clear_during_read = true;
    assert(d1l_ui_packet_query_submit(&request, 4U) == ESP_OK);
    process_request();
    assert(d1l_ui_packet_query_take(&result) && result.error != ESP_OK && result.count == 0U);
    assert(d1l_ui_packet_query_submit(&request, 5U) == ESP_OK);
    process_request();
    ++s_stats.sd_backend_generation;
    assert(d1l_ui_packet_query_take(&result) && result.error != ESP_OK && result.count == 0U);

    assert(d1l_ui_packet_query_submit(&request, 6U) == ESP_OK);
    const unsigned calls = s_calls;
    d1l_ui_packet_query_cancel();
    process_request();
    assert(s_calls == calls && !d1l_ui_packet_query_ready() && !d1l_ui_packet_query_pending());
    puts("native asynchronous packet query: ok");
    return 0;
}
