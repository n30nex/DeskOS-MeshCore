#pragma once

#include "ui_packets.h"

typedef struct {
    esp_err_t error;
    size_t count;
    bool has_more;
    bool sd_used;
    uint32_t query_generation;
    uint32_t sd_generation;
    d1l_packet_log_entry_t rows[D1L_UI_PACKETS_INITIAL_ROWS];
} d1l_ui_packet_query_result_t;

/* UI-owner calls only; the worker owns storage reads and never calls LVGL.
 * A changed request supersedes an in-flight scan. Repeated renders coalesce. */
esp_err_t d1l_ui_packet_query_submit(const d1l_ui_packets_query_request_t *request,
                                    uint64_t revision);
bool d1l_ui_packet_query_pending(void);
bool d1l_ui_packet_query_ready(void);
bool d1l_ui_packet_query_take(d1l_ui_packet_query_result_t *result);
void d1l_ui_packet_query_cancel(void);
