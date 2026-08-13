#pragma once

#include <stdbool.h>
#include <stdint.h>

#define D1L_UI_BOOT_SCENE_DURATION_MS 3200U
#define D1L_UI_BOOT_SCENE_READY_MS 2800U

typedef enum {
    D1L_UI_BOOT_PHASE_STARTING = 0,
    D1L_UI_BOOT_PHASE_LINKING,
    D1L_UI_BOOT_PHASE_PREPARING,
    D1L_UI_BOOT_PHASE_READY,
    D1L_UI_BOOT_PHASE_COMPLETE,
} d1l_ui_boot_phase_t;

typedef struct {
    d1l_ui_boot_phase_t phase;
    uint8_t frame_opacity;
    uint8_t title_opacity;
    uint8_t node_count;
    uint8_t link_count;
    uint8_t pulse_opacity;
    uint8_t spark_percent;
    uint8_t progress_percent;
    bool complete;
} d1l_ui_boot_scene_state_t;

void d1l_ui_boot_scene_state_at(uint32_t elapsed_ms,
                                d1l_ui_boot_scene_state_t *out_state);
const char *d1l_ui_boot_phase_text(d1l_ui_boot_phase_t phase);
