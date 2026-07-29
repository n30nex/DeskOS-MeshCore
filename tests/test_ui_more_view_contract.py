from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_more_truth_and_event_lifetime_are_owned_outside_phase1():
    cmake = read("main/CMakeLists.txt")
    phase1 = read("main/ui/ui_phase1.c")
    renderer = read("main/ui/ui_settings.c")
    renderer_header = read("main/ui/ui_settings.h")
    view = read("main/ui/ui_more_view.c")

    assert '"ui/ui_more_view.c"' in cmake
    assert "d1l_ui_more_view_input_t" in phase1
    assert "more_view_input_from_snapshot" in phase1
    assert "d1l_ui_more_view(&input, &s_settings_controller.rendered)" in phase1
    assert "d1l_ui_more_view_model_t view_model" not in phase1
    assert "d1l_ui_settings_controller_t s_settings_controller EXT_RAM_BSS_ATTR" in phase1
    assert "d1l_ui_settings_deactivate(&s_settings_controller)" in phase1
    assert "d1l_ui_settings_action_binding_t" in renderer_header
    assert "d1l_ui_settings_category_binding_t" in renderer_header
    assert "binding->generation != controller->generation" in renderer
    assert "binding->item_index >= category->item_count" in renderer
    assert "item->action == binding->action" in renderer
    assert "controller->rendered = *view_model" in renderer
    assert "view_model != &controller->rendered" in renderer
    assert "controller->active" in renderer
    assert "D1L_UI_SETTINGS_CONTROLLER_MAX_BYTES" in renderer_header
    assert "_Static_assert(sizeof(d1l_ui_settings_controller_t)" in renderer

    assert "lv_" not in view
    assert "d1l_app_model" not in view
    assert "freertos/" not in view
    assert "d1l_app_snapshot_t" not in renderer
    assert "snapshot->" not in renderer
    assert "static d1l_ui_settings_action_handler_t s_action_handler" not in renderer
    assert "s_expanded_category" not in renderer
    assert "k_action_values" not in renderer


def test_more_view_models_all_bounded_categories_and_truth_states():
    view = read("main/ui/ui_more_view.c")
    header = read("main/ui/ui_more_view.h")

    for category in (
        "Tools",
        "Connections",
        "Storage & maps",
        "Device",
        "Support",
        "Advanced",
    ):
        assert f'"{category}"' in view
    for state in (
        "Unavailable",
        "Connected",
        "Connecting",
        "Applying",
        "Needs setup",
        "Needs attention",
        "Reconnecting",
        "Needs Wi-Fi",
        "Loading",
        "Time setting unavailable",
    ):
        assert f'"{state}"' in view
    assert "D1L_UI_MORE_MAX_ITEMS_PER_CATEGORY" in header
    assert "d1l_ui_more_view_model_is_valid" in view
    assert "item->actionable != action_valid" in view
    assert "bounded_text_is_terminated" in view
    assert "memchr(text, '\\0', capacity)" in view
    assert "D1L_UI_MORE_VIEW_MODEL_MAX_BYTES" in header
    assert "_Static_assert(sizeof(d1l_ui_more_view_model_t)" in view
    assert "timezone_settings_ready" in header
    assert "timezone_label" in header
    assert "time_label" in header


def test_more_renderer_preserves_geometry_and_rejects_stale_events():
    renderer = read("main/ui/ui_settings.c")

    assert "settings_create_row(parent, 444, 54, item->warning)" in renderer
    assert "settings_create_container(group, 444)" in renderer
    assert "lv_obj_set_height(category_row, 34)" in renderer
    assert "settings_set_dot_width(title_label, 410)" in renderer
    assert "lv_obj_set_pos(controller->menu, 18, 54)" in renderer
    assert "settings_set_dot_width(title, 190)" in renderer
    assert "settings_set_dot_width(status, item->actionable ? 176 : 202)" in renderer
    assert "binding->generation != controller->generation" in renderer
    assert "controller->generation == 0U" in renderer
    assert "!controller->active" in renderer
    assert "d1l_ui_more_view_model_is_valid(view_model)" in renderer
    assert "controller->expanded_category = D1L_UI_MORE_CATEGORY_NONE" in renderer
