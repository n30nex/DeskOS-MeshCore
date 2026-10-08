#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh/packet_log.h"

#define D1L_UI_PACKETS_INITIAL_ROWS 12U
#define D1L_UI_PACKETS_LOAD_NEWER_STEP 12U

typedef enum {
    D1L_UI_PACKET_FILTER_ALL = 0,
    D1L_UI_PACKET_FILTER_RX,
    D1L_UI_PACKET_FILTER_TX,
    D1L_UI_PACKET_FILTER_TEXT,
    D1L_UI_PACKET_FILTER_COUNT,
} d1l_ui_packet_filter_t;

typedef struct {
    d1l_ui_packet_filter_t filter;
    bool paused;
    bool sd_history_page;
    char search_text[D1L_PACKET_LOG_QUERY_TEXT_LEN];
    size_t row_limit;
    size_t skip_newest;
    size_t total_matches;
    size_t row_count;
    d1l_packet_log_entry_t rows[D1L_PACKET_LOG_CAPACITY];
} d1l_ui_packets_controller_t;

typedef struct {
    size_t row_limit;
    size_t skip_newest;
    const char *direction;
    const char *kind;
    const char *search_text;
} d1l_ui_packets_query_request_t;

void d1l_ui_packets_init(d1l_ui_packets_controller_t *controller);
void d1l_ui_packets_select_filter(d1l_ui_packets_controller_t *controller,
                                  d1l_ui_packet_filter_t filter);
void d1l_ui_packets_toggle_pause(d1l_ui_packets_controller_t *controller);
void d1l_ui_packets_set_search(d1l_ui_packets_controller_t *controller,
                               const char *search_text);
void d1l_ui_packets_clear_search(d1l_ui_packets_controller_t *controller);
void d1l_ui_packets_load_older(d1l_ui_packets_controller_t *controller);
void d1l_ui_packets_load_newer(d1l_ui_packets_controller_t *controller);
bool d1l_ui_packets_query_request(d1l_ui_packets_controller_t *controller,
                                  d1l_ui_packets_query_request_t *request);
bool d1l_ui_packets_accept_query(d1l_ui_packets_controller_t *controller,
                                 const d1l_packet_log_entry_t *rows,
                                 size_t row_count,
                                 size_t total_matches,
                                 bool sd_history_page);
bool d1l_ui_packets_can_load_older(const d1l_ui_packets_controller_t *controller);
bool d1l_ui_packets_can_load_newer(const d1l_ui_packets_controller_t *controller);
const char *d1l_ui_packets_filter_direction(const d1l_ui_packets_controller_t *controller);
const char *d1l_ui_packets_filter_kind(const d1l_ui_packets_controller_t *controller);
uint32_t d1l_ui_packets_entry_color(const d1l_packet_log_entry_t *entry);
const char *d1l_ui_packets_entry_status(const d1l_packet_log_entry_t *entry);
