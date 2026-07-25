#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define D1L_UI_CAPTURE_WIDTH 480U
#define D1L_UI_CAPTURE_HEIGHT 480U
#define D1L_UI_CAPTURE_BYTES_PER_PIXEL 2U
#define D1L_UI_CAPTURE_TOTAL_BYTES (D1L_UI_CAPTURE_WIDTH * D1L_UI_CAPTURE_HEIGHT * D1L_UI_CAPTURE_BYTES_PER_PIXEL)
#define D1L_UI_CAPTURE_MAX_CHUNK_BYTES 1024U

typedef struct {
    bool ok;
    bool surface_supported;
    bool target_found;
    bool scrollable;
    bool movement_required;
    bool moved;
    char surface[24];
    char tab[16];
    int32_t before_y;
    int32_t after_y;
    int32_t scroll_top_before;
    int32_t scroll_bottom_before;
    int32_t scroll_top_after;
    int32_t scroll_bottom_after;
} d1l_ui_scroll_probe_result_t;

typedef struct {
    bool available;
    bool active;
    bool shadow_ready;
    bool started;
    bool onboarding_visible;
    bool lock_visible;
    bool tab_pending;
    bool content_pending;
    bool render_pending;
    uint16_t width;
    uint16_t height;
    uint8_t bytes_per_pixel;
    size_t total_bytes;
    size_t max_chunk_bytes;
    uint32_t frame_seq;
    uint32_t flush_count;
    uint32_t capture_crc32;
    char active_tab[16];
    char pending_tab[16];
} d1l_ui_capture_status_t;

typedef struct {
    bool ok;
    bool target_supported;
    bool sheet_visible;
    bool textarea_visible;
    bool keyboard_visible;
    bool onboarding_visible;
    bool dock_hidden;
    bool dm_mode;
    bool tx_suppressed;
    bool send_enabled;
    char target[16];
    char active_tab[16];
    int32_t sheet_x;
    int32_t sheet_y;
    int32_t sheet_w;
    int32_t sheet_h;
    int32_t textarea_x;
    int32_t textarea_y;
    int32_t textarea_w;
    int32_t textarea_h;
    int32_t keyboard_x;
    int32_t keyboard_y;
    int32_t keyboard_w;
    int32_t keyboard_h;
} d1l_ui_compose_probe_result_t;

esp_err_t d1l_ui_phase1_start(void);
bool d1l_ui_phase1_started(void);
esp_err_t d1l_ui_phase1_show_home(void);
esp_err_t d1l_ui_phase1_request_tab(const char *name);
esp_err_t d1l_ui_phase1_scroll_probe(const char *surface,
                                      d1l_ui_scroll_probe_result_t *result);
esp_err_t d1l_ui_phase1_compose_probe(const char *target,
                                       d1l_ui_compose_probe_result_t *result);
const char *d1l_ui_phase1_active_tab_name(void);
const char *d1l_ui_phase1_pending_tab_name(void);
bool d1l_ui_phase1_tab_switch_pending(void);
esp_err_t d1l_ui_capture_status(d1l_ui_capture_status_t *out_status);
esp_err_t d1l_ui_capture_begin(d1l_ui_capture_status_t *out_status);
esp_err_t d1l_ui_capture_chunk(size_t offset,
                               size_t requested_len,
                               uint8_t *out_data,
                               size_t out_capacity,
                               size_t *out_len,
                               uint32_t *out_crc32,
                               d1l_ui_capture_status_t *out_status);
esp_err_t d1l_ui_capture_end(d1l_ui_capture_status_t *out_status);
