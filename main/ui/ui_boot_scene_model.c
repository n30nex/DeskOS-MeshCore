#include "ui_boot_scene_model.h"

#include <stddef.h>

static uint8_t ramp_u8(uint32_t value, uint32_t start, uint32_t end)
{
    if (value <= start) {
        return 0U;
    }
    if (value >= end || end <= start) {
        return UINT8_MAX;
    }
    return (uint8_t)(((value - start) * UINT8_MAX) / (end - start));
}

void d1l_ui_boot_scene_state_at(uint32_t elapsed_ms,
                                d1l_ui_boot_scene_state_t *out_state)
{
    if (!out_state) {
        return;
    }

    const uint32_t capped = elapsed_ms > D1L_UI_BOOT_SCENE_READY_MS ?
        D1L_UI_BOOT_SCENE_READY_MS : elapsed_ms;
    const uint32_t pulse = (elapsed_ms / 4U) % 256U;
    *out_state = (d1l_ui_boot_scene_state_t) {
        .phase = elapsed_ms < 600U ? D1L_UI_BOOT_PHASE_STARTING :
            elapsed_ms < 1500U ? D1L_UI_BOOT_PHASE_LINKING :
            elapsed_ms < D1L_UI_BOOT_SCENE_READY_MS ?
                D1L_UI_BOOT_PHASE_PREPARING :
            elapsed_ms < D1L_UI_BOOT_SCENE_DURATION_MS ?
                D1L_UI_BOOT_PHASE_READY : D1L_UI_BOOT_PHASE_COMPLETE,
        .frame_opacity = ramp_u8(elapsed_ms, 40U, 520U),
        .title_opacity = ramp_u8(elapsed_ms, 1050U, 1600U),
        .node_count = elapsed_ms < 420U ? 0U :
            elapsed_ms < 650U ? 1U : elapsed_ms < 880U ? 2U : 3U,
        .link_count = elapsed_ms < 760U ? 0U :
            elapsed_ms < 980U ? 1U : elapsed_ms < 1200U ? 2U : 3U,
        .pulse_opacity = (uint8_t)(160U +
            (pulse <= 127U ? pulse : 255U - pulse) * 95U / 127U),
        .spark_percent = elapsed_ms < 650U ? 0U :
            (uint8_t)(((elapsed_ms - 650U) / 18U) % 100U),
        .progress_percent = (uint8_t)((capped * 100U) /
            D1L_UI_BOOT_SCENE_READY_MS),
        .complete = elapsed_ms >= D1L_UI_BOOT_SCENE_DURATION_MS,
    };
}

const char *d1l_ui_boot_phase_text(d1l_ui_boot_phase_t phase)
{
    switch (phase) {
    case D1L_UI_BOOT_PHASE_STARTING:
        return "Starting DeskOS";
    case D1L_UI_BOOT_PHASE_LINKING:
        return "Drawing the mesh";
    case D1L_UI_BOOT_PHASE_PREPARING:
        return "Opening your desk";
    case D1L_UI_BOOT_PHASE_READY:
    case D1L_UI_BOOT_PHASE_COMPLETE:
        return "Ready";
    default:
        return "Starting DeskOS";
    }
}
