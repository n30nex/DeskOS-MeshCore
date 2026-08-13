#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui_packets.h"

static size_t s_query_calls;
static size_t s_total_rows = 250U;
static bool s_use_sd = true;
static uint32_t s_generation = 1U;
static char s_direction[8];
static char s_kind[8];
static char s_search[D1L_PACKET_LOG_QUERY_TEXT_LEN];

static size_t perform_query(d1l_ui_packets_controller_t *controller)
{
    static d1l_packet_log_entry_t query_rows[D1L_PACKET_LOG_CAPACITY];
    for (size_t attempt = 0U; attempt < 2U; ++attempt) {
        d1l_ui_packets_query_request_t request;
        if (!d1l_ui_packets_query_request(controller, &request)) {
            break;
        }
        s_query_calls++;
        snprintf(s_direction, sizeof(s_direction), "%s", request.direction);
        snprintf(s_kind, sizeof(s_kind), "%s", request.kind);
        snprintf(s_search, sizeof(s_search), "%s", request.search_text);
        size_t count = 0U;
        if (request.skip_newest < s_total_rows) {
            count = s_total_rows - request.skip_newest;
            if (count > request.row_limit) {
                count = request.row_limit;
            }
        }
        for (size_t index = 0U; index < count; ++index) {
            memset(&query_rows[index], 0, sizeof(query_rows[index]));
            query_rows[index].seq =
                (s_generation * 1000U) + (uint32_t)request.skip_newest + (uint32_t)index;
        }
        if (!d1l_ui_packets_accept_query(controller, query_rows, count,
                                         s_total_rows, s_use_sd)) {
            break;
        }
    }
    return controller->row_count;
}

static void test_filter_search_and_page_state(void)
{
    d1l_ui_packets_controller_t controller;
    d1l_ui_packets_init(&controller);
    assert(controller.filter == D1L_UI_PACKET_FILTER_ALL);
    assert(controller.row_limit == D1L_UI_PACKETS_INITIAL_ROWS);
    assert(!controller.paused);

    assert(perform_query(&controller) == 100U);
    assert(strcmp(s_direction, "any") == 0);
    assert(strcmp(s_kind, "any") == 0);
    assert(controller.total_matches == 250U);
    assert(controller.sd_history_page);
    assert(d1l_ui_packets_can_load_older(&controller));
    assert(!d1l_ui_packets_can_load_newer(&controller));

    d1l_ui_packets_select_filter(&controller, D1L_UI_PACKET_FILTER_RX);
    assert(perform_query(&controller) == 100U);
    assert(strcmp(s_direction, "rx") == 0);
    d1l_ui_packets_select_filter(&controller, D1L_UI_PACKET_FILTER_TEXT);
    assert(perform_query(&controller) == 100U);
    assert(strcmp(s_direction, "any") == 0);
    assert(strcmp(s_kind, "text") == 0);

    d1l_ui_packets_set_search(&controller, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    assert(strlen(controller.search_text) == D1L_PACKET_LOG_QUERY_TEXT_LEN - 1U);
    assert(controller.skip_newest == 0U);
    assert(!controller.paused);
    perform_query(&controller);
    assert(strcmp(s_search, controller.search_text) == 0);

    d1l_ui_packets_load_older(&controller);
    assert(controller.skip_newest == 100U);
    assert(d1l_ui_packets_can_load_newer(&controller));
    perform_query(&controller);
    d1l_ui_packets_load_older(&controller);
    assert(controller.skip_newest == 200U);
    assert(perform_query(&controller) == 50U);
    assert(!d1l_ui_packets_can_load_older(&controller));
    d1l_ui_packets_load_newer(&controller);
    assert(controller.skip_newest == 100U);
    d1l_ui_packets_load_newer(&controller);
    assert(controller.skip_newest == 0U);

    d1l_ui_packets_select_filter(&controller, D1L_UI_PACKET_FILTER_COUNT);
    assert(controller.filter == D1L_UI_PACKET_FILTER_ALL);
    d1l_ui_packets_clear_search(&controller);
    assert(controller.search_text[0] == '\0');
}

static void test_pause_is_a_stable_snapshot_and_empty_pages_recover(void)
{
    d1l_ui_packets_controller_t controller;
    d1l_ui_packets_init(&controller);
    perform_query(&controller);
    perform_query(&controller);
    d1l_ui_packets_toggle_pause(&controller);
    assert(controller.paused);
    const size_t calls_at_pause = s_query_calls;
    const uint32_t paused_seq = controller.rows[0].seq;
    s_generation++;
    assert(perform_query(&controller) == controller.row_count);
    assert(s_query_calls == calls_at_pause);
    assert(controller.rows[0].seq == paused_seq);

    d1l_ui_packets_toggle_pause(&controller);
    assert(!controller.paused);
    perform_query(&controller);
    assert(controller.rows[0].seq != paused_seq);

    controller.skip_newest = 999U;
    const size_t before_fallback = s_query_calls;
    assert(perform_query(&controller) == 100U);
    assert(controller.skip_newest == 0U);
    assert(s_query_calls == before_fallback + 2U);
}

static void test_packet_presentation_is_deterministic(void)
{
    d1l_packet_log_entry_t entry = {0};
    snprintf(entry.direction, sizeof(entry.direction), "RX");
    assert(strcmp(d1l_ui_packets_entry_status(&entry), "RX") == 0);
    assert(d1l_ui_packets_entry_color(&entry) == 0x20D9EDU);

    snprintf(entry.direction, sizeof(entry.direction), "tx");
    snprintf(entry.kind, sizeof(entry.kind), "control");
    assert(strcmp(d1l_ui_packets_entry_status(&entry), "TX") == 0);
    assert(d1l_ui_packets_entry_color(&entry) == 0xFBBF24U);

    snprintf(entry.note, sizeof(entry.note), "Hard ERROR from radio");
    assert(strcmp(d1l_ui_packets_entry_status(&entry), "ERR") == 0);
    assert(d1l_ui_packets_entry_color(&entry) == 0xF87171U);
    assert(strcmp(d1l_ui_packets_entry_status(NULL), "UNK") == 0);
}

int main(void)
{
    test_filter_search_and_page_state();
    test_pause_is_a_stable_snapshot_and_empty_pages_recover();
    test_packet_presentation_is_deterministic();
    puts("native UI packets controller: ok");
    return 0;
}
