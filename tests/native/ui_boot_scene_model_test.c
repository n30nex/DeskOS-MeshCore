#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui_boot_scene_model.h"

static void test_timeline_is_monotonic_and_bounded(void)
{
    d1l_ui_boot_scene_state_t previous = {0};
    for (uint32_t elapsed = 0U;
         elapsed <= D1L_UI_BOOT_SCENE_DURATION_MS;
         elapsed += 40U) {
        d1l_ui_boot_scene_state_t state = {0};
        d1l_ui_boot_scene_state_at(elapsed, &state);
        assert(state.frame_opacity >= previous.frame_opacity);
        assert(state.title_opacity >= previous.title_opacity);
        assert(state.node_count >= previous.node_count);
        assert(state.link_count >= previous.link_count);
        assert(state.progress_percent >= previous.progress_percent);
        assert(state.node_count <= 3U);
        assert(state.link_count <= 3U);
        assert(state.progress_percent <= 100U);
        previous = state;
    }
}

static void test_user_facing_phases_and_completion(void)
{
    d1l_ui_boot_scene_state_t state = {0};
    d1l_ui_boot_scene_state_at(0U, &state);
    assert(state.phase == D1L_UI_BOOT_PHASE_STARTING);
    assert(strcmp(d1l_ui_boot_phase_text(state.phase), "Starting DeskOS") == 0);

    d1l_ui_boot_scene_state_at(900U, &state);
    assert(state.phase == D1L_UI_BOOT_PHASE_LINKING);
    assert(strcmp(d1l_ui_boot_phase_text(state.phase), "Drawing the mesh") == 0);
    assert(state.node_count == 3U);
    assert(state.link_count == 1U);

    d1l_ui_boot_scene_state_at(2000U, &state);
    assert(state.phase == D1L_UI_BOOT_PHASE_PREPARING);
    assert(strcmp(d1l_ui_boot_phase_text(state.phase), "Opening your desk") == 0);
    assert(state.node_count == 3U);
    assert(state.link_count == 3U);

    d1l_ui_boot_scene_state_at(D1L_UI_BOOT_SCENE_READY_MS, &state);
    assert(state.phase == D1L_UI_BOOT_PHASE_READY);
    assert(state.progress_percent == 100U);
    assert(!state.complete);

    d1l_ui_boot_scene_state_at(D1L_UI_BOOT_SCENE_DURATION_MS, &state);
    assert(state.phase == D1L_UI_BOOT_PHASE_COMPLETE);
    assert(state.complete);
    assert(strcmp(d1l_ui_boot_phase_text(state.phase), "Ready") == 0);
}

int main(void)
{
    test_timeline_is_monotonic_and_bounded();
    test_user_facing_phases_and_completion();
    puts("native UI boot scene model: ok");
    return 0;
}
