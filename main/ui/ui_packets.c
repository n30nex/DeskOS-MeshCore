#include "ui_packets.h"

#include <stdio.h>
#include <string.h>

static char packets_lower_ascii(char value)
{
    return (value >= 'A' && value <= 'Z') ? (char)(value + ('a' - 'A')) : value;
}

static bool packets_text_has_token(const char *text, const char *token)
{
    if (!text || !token || token[0] == '\0') {
        return false;
    }
    for (const char *cursor = text; *cursor; ++cursor) {
        const char *left = cursor;
        const char *right = token;
        while (*left && *right && packets_lower_ascii(*left) == packets_lower_ascii(*right)) {
            ++left;
            ++right;
        }
        if (*right == '\0') {
            return true;
        }
    }
    return false;
}

static size_t packets_normalize_row_limit(size_t row_limit)
{
    if (row_limit < D1L_UI_PACKETS_INITIAL_ROWS) {
        return D1L_UI_PACKETS_INITIAL_ROWS;
    }
    if (row_limit > D1L_PACKET_LOG_CAPACITY) {
        return D1L_PACKET_LOG_CAPACITY;
    }
    return row_limit;
}

const char *d1l_ui_packets_filter_direction(const d1l_ui_packets_controller_t *controller)
{
    if (!controller) {
        return "any";
    }
    switch (controller->filter) {
    case D1L_UI_PACKET_FILTER_RX:
        return "rx";
    case D1L_UI_PACKET_FILTER_TX:
        return "tx";
    case D1L_UI_PACKET_FILTER_ALL:
    case D1L_UI_PACKET_FILTER_TEXT:
    case D1L_UI_PACKET_FILTER_COUNT:
    default:
        return "any";
    }
}

const char *d1l_ui_packets_filter_kind(const d1l_ui_packets_controller_t *controller)
{
    return controller && controller->filter == D1L_UI_PACKET_FILTER_TEXT ? "text" : "any";
}

void d1l_ui_packets_init(d1l_ui_packets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    memset(controller, 0, sizeof(*controller));
    controller->filter = D1L_UI_PACKET_FILTER_ALL;
    controller->row_limit = D1L_UI_PACKETS_INITIAL_ROWS;
}

void d1l_ui_packets_select_filter(d1l_ui_packets_controller_t *controller,
                                  d1l_ui_packet_filter_t filter)
{
    if (!controller) {
        return;
    }
    controller->filter = filter < D1L_UI_PACKET_FILTER_COUNT ? filter : D1L_UI_PACKET_FILTER_ALL;
    controller->row_limit = D1L_UI_PACKETS_INITIAL_ROWS;
    controller->skip_newest = 0U;
    controller->paused = false;
}

void d1l_ui_packets_toggle_pause(d1l_ui_packets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    if (controller->paused) {
        controller->paused = false;
        return;
    }
    controller->paused = true;
}

void d1l_ui_packets_set_search(d1l_ui_packets_controller_t *controller,
                               const char *search_text)
{
    if (!controller) {
        return;
    }
    snprintf(controller->search_text, sizeof(controller->search_text), "%s",
             search_text ? search_text : "");
    controller->row_limit = D1L_UI_PACKETS_INITIAL_ROWS;
    controller->skip_newest = 0U;
    controller->paused = false;
}

void d1l_ui_packets_clear_search(d1l_ui_packets_controller_t *controller)
{
    d1l_ui_packets_set_search(controller, "");
}

void d1l_ui_packets_load_older(d1l_ui_packets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    if (controller->row_count > 0U &&
        controller->total_matches > controller->skip_newest + controller->row_count) {
        controller->skip_newest += controller->row_count;
    }
    controller->paused = false;
}

void d1l_ui_packets_load_newer(d1l_ui_packets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    if (controller->skip_newest > D1L_UI_PACKETS_LOAD_NEWER_STEP) {
        controller->skip_newest -= D1L_UI_PACKETS_LOAD_NEWER_STEP;
    } else {
        controller->skip_newest = 0U;
    }
    controller->paused = false;
}

bool d1l_ui_packets_query_request(d1l_ui_packets_controller_t *controller,
                                  d1l_ui_packets_query_request_t *request)
{
    if (!controller || !request || controller->paused) {
        return false;
    }
    controller->row_limit = packets_normalize_row_limit(controller->row_limit);
    *request = (d1l_ui_packets_query_request_t) {
        .row_limit = controller->row_limit,
        .skip_newest = controller->skip_newest,
        .direction = d1l_ui_packets_filter_direction(controller),
        .kind = d1l_ui_packets_filter_kind(controller),
        .search_text = controller->search_text,
    };
    return true;
}

bool d1l_ui_packets_accept_query(d1l_ui_packets_controller_t *controller,
                                 const d1l_packet_log_entry_t *rows,
                                 size_t row_count,
                                 size_t total_matches,
                                 bool sd_history_page)
{
    if (!controller || controller->paused) {
        return false;
    }
    controller->row_count = row_count > controller->row_limit ? controller->row_limit : row_count;
    if (controller->row_count > 0U) {
        if (!rows) {
            controller->row_count = 0U;
        } else {
            memcpy(controller->rows, rows,
                   controller->row_count * sizeof(controller->rows[0]));
        }
    }
    controller->total_matches = total_matches;
    controller->sd_history_page = sd_history_page;
    if (controller->row_count == 0U && controller->skip_newest > 0U) {
        controller->skip_newest = 0U;
        return true;
    }
    return false;
}

bool d1l_ui_packets_can_load_older(const d1l_ui_packets_controller_t *controller)
{
    return controller && controller->row_count > 0U &&
           controller->total_matches > controller->skip_newest + controller->row_count;
}

bool d1l_ui_packets_can_load_newer(const d1l_ui_packets_controller_t *controller)
{
    return controller && controller->skip_newest > 0U;
}

uint32_t d1l_ui_packets_entry_color(const d1l_packet_log_entry_t *entry)
{
    if (!entry) {
        return 0xA6B0B7;
    }
    if (packets_text_has_token(entry->kind, "fail") ||
        packets_text_has_token(entry->note, "fail") ||
        packets_text_has_token(entry->kind, "error") ||
        packets_text_has_token(entry->note, "error")) {
        return 0xF87171;
    }
    if (packets_text_has_token(entry->kind, "raw") ||
        packets_text_has_token(entry->kind, "control")) {
        return 0xFBBF24;
    }
    if (packets_text_has_token(entry->direction, "tx")) {
        return 0x4D7FFF;
    }
    if (packets_text_has_token(entry->direction, "rx")) {
        return 0x20D9ED;
    }
    return 0x84FF2E;
}

const char *d1l_ui_packets_entry_status(const d1l_packet_log_entry_t *entry)
{
    if (!entry) {
        return "UNK";
    }
    if (packets_text_has_token(entry->kind, "fail") ||
        packets_text_has_token(entry->note, "fail")) {
        return "FAIL";
    }
    if (packets_text_has_token(entry->kind, "error") ||
        packets_text_has_token(entry->note, "error")) {
        return "ERR";
    }
    if (packets_text_has_token(entry->direction, "tx")) {
        return "TX";
    }
    if (packets_text_has_token(entry->direction, "rx")) {
        return "RX";
    }
    return "LOG";
}
