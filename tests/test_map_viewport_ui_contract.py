from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def test_map_ui_uses_bounded_core_viewport_and_visible_attribution():
    source = read("main/ui/ui_map.c")
    header = read("main/ui/ui_map.h")
    math_header = read("main/map/map_math.h")

    assert '#include "map/map_view_service.h"' in source
    assert '#include "ui_chrome.h"' in source
    assert "D1L_MAP_VIEW_DEFAULT_ZOOM 10U" in math_header
    assert "D1L_MAP_VIEW_MIN_ZOOM 8U" in math_header
    assert "D1L_MAP_VIEW_MAX_ZOOM 18U" in math_header
    assert "s_viewport_zoom = D1L_MAP_VIEW_DEFAULT_ZOOM" in source
    assert "MAP_VIEWPORT_WIDTH 478U" in source
    assert "MAP_VIEWPORT_HEIGHT (D1L_UI_DOCKED_CONTENT_HEIGHT - 2U)" in source
    assert "MAP_VIEWPORT_PANEL_HEIGHT (MAP_VIEWPORT_HEIGHT + 2U)" in source
    assert "parent, 0, 0, 480, MAP_VIEWPORT_PANEL_HEIGHT" in source
    assert "d1l_map_view_service_acquire_visible" in source
    assert "d1l_map_view_service_acquire_frame" in source
    assert "d1l_map_view_service_release_frame" in source
    assert "d1l_map_view_service_release_visible" in source
    assert "lv_img_cache_invalidate_src(&s_viewport_image)" in source
    assert "lv_obj_invalidate(s_viewport_image_obj)" in source
    assert '"(c) OpenStreetMap contributors"' in source
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in source
    ensure_pixels = function_body(
        source, "static bool map_viewport_ensure_pixels", "static void map_viewport_apply_service_status"
    )
    assert ensure_pixels.count("heap_caps_malloc") == 1
    assert "heap_caps_realloc" not in ensure_pixels

    for api in (
        "d1l_ui_map_viewport_release",
        "d1l_ui_map_viewport_set_suppressed",
        "d1l_ui_map_viewport_suppress_next_acquire",
        "d1l_ui_map_viewport_refresh",
        "d1l_ui_map_viewport_reacquire",
        "d1l_ui_map_viewport_prepare_cover",
        "d1l_ui_map_viewport_lease_active",
        "d1l_ui_map_viewport_generation",
    ):
        assert api in header
        assert api in source


def test_map_viewport_touch_controls_pan_on_release_and_request_one_new_view():
    source = read("main/ui/ui_map.c")
    render = function_body(
        source, "void d1l_ui_map_render(", "static void map_render_options_root"
    )
    drag = function_body(
        source,
        "static void map_viewport_drag_event_cb",
        "static void map_viewport_zoom_in_event_cb",
    )
    pressing = drag.split("if (code == LV_EVENT_PRESSING", 1)[1].split(
        "if (code == LV_EVENT_PRESS_LOST", 1
    )[0]
    released = drag.split("if (code != LV_EVENT_RELEASED", 1)[1]
    zoom_in = function_body(
        source,
        "static void map_viewport_zoom_in_event_cb",
        "static void map_viewport_zoom_out_event_cb",
    )
    zoom_out = function_body(
        source,
        "static void map_viewport_zoom_out_event_cb",
        "static void map_viewport_recenter_event_cb",
    )
    recenter = function_body(
        source,
        "static void map_viewport_recenter_event_cb",
        "bool d1l_ui_map_viewport_refresh",
    )

    assert 'viewport, "Options", 8, 8, 96, 48' in render
    assert (
        'viewport, "Center", 8, (int)MAP_VIEWPORT_HEIGHT - 60, 96, 52'
        in render
    )
    assert 'viewport, "-", 420, 92, 52, 52' in render
    assert 'viewport, "+", 420, 8, 52, 52' in render
    assert "map_style_overlay_button" in render
    assert "LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN" in render
    assert "lv_obj_set_scroll_dir(parent, LV_DIR_NONE)" in render
    assert "lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF)" in render
    assert 'map_label(viewport, "Drag to pan"' in render
    assert "s_viewport_drag_hint_label = drag_hint" in render
    assert "s_viewport_progress_bar = lv_bar_create(viewport)" in render
    assert "lv_obj_set_size(s_viewport_progress_bar, 280, 8)" in render
    assert "lv_bar_set_range(s_viewport_progress_bar, 0," in render
    assert "lv_obj_clear_flag(s_viewport_progress_bar, LV_OBJ_FLAG_CLICKABLE)" in render
    assert "lv_obj_add_flag(viewport, LV_OBJ_FLAG_CLICKABLE)" in render
    assert "LV_OBJ_FLAG_GESTURE_BUBBLE" in render
    for event in (
        "LV_EVENT_PRESSED",
        "LV_EVENT_PRESSING",
        "LV_EVENT_RELEASED",
        "LV_EVENT_PRESS_LOST",
    ):
        assert f"map_viewport_drag_event_cb, {event}" in render

    assert "lv_indev_get_vect" in pressing
    assert "map_drag_clamp" in pressing
    assert "MAP_DRAG_MAX_X_PIXELS" in pressing
    assert "MAP_DRAG_MAX_Y_PIXELS" in pressing
    assert "portENTER_CRITICAL(&s_viewport_lease_lock)" in pressing
    assert "lv_obj_set_pos(s_viewport_image_obj" in pressing
    assert "lv_obj_set_pos(s_viewport_marker_layer" in pressing
    assert "map_viewport_request_interactive_view" not in pressing
    assert "MAP_DRAG_THRESHOLD_PIXELS" in released
    assert "d1l_map_math_pan_center" in released
    assert "map_viewport_request_interactive_view()" in released

    assert "map_zoom_clamp" in zoom_in
    assert "s_viewport_zoom + 1U" in zoom_in
    assert "map_viewport_request_interactive_view()" in zoom_in
    assert "D1L_MAP_VIEW_MIN_ZOOM" in zoom_out
    assert "--s_viewport_zoom" in zoom_out
    assert "map_viewport_request_interactive_view()" in zoom_out
    assert "s_viewport_lat_e7 = s_viewport_saved_lat_e7" in recenter
    assert "s_viewport_lon_e7 = s_viewport_saved_lon_e7" in recenter
    assert "map_viewport_request_interactive_view()" in recenter

    assert (
        "s_viewport_lat_e7, s_viewport_lon_e7, s_viewport_zoom" in render
    )
    status = function_body(
        source,
        "static void map_viewport_apply_service_status",
        "static void map_viewport_update_controls",
    )
    assert 'title = "Waiting for storage"' in status
    assert 'detail = "The card reader is starting or checking the SD card."' in status
    assert 'lv_label_set_text(s_viewport_drag_hint_label, "Storage starting")' in status

    initial = function_body(
        source,
        "static void map_viewport_copy",
        "static int32_t map_drag_clamp",
    )
    assert 'map_storage_needs_attention(snapshot)' in initial
    assert '*title = "SD card required";' in initial
    assert '*title = "FAT32 card required";' in initial
    assert '*title = "Waiting for storage";' in initial
    assert '"Insert a FAT32 card, or wait while it prepares."' not in initial
    assert 'lv_label_set_text(s_viewport_drag_hint_label, "Waiting for Wi-Fi")' in status
    assert 'lv_label_set_text(s_viewport_drag_hint_label, "Checking secure time")' in status
    assert 'lv_label_set_text(s_viewport_drag_hint_label, "Map paused")' in status
    for phase in (
        '"sd_attention"',
        '"sd_card_required"',
        '"sd_fat32_required"',
        '"sd_reconnecting"',
        '"sd_cache_required"',
    ):
        assert phase in status
    assert "status->rendered_tiles + status->failed_tiles" in status
    assert "lv_bar_set_value(s_viewport_progress_bar, completed, LV_ANIM_OFF)" in status
    assert 'strcmp(status->phase, "loading_cache") == 0' in status
    assert 'strcmp(status->phase, "downloading") == 0' in status
    assert "lv_obj_clear_flag(s_viewport_progress_bar, LV_OBJ_FLAG_HIDDEN)" in status
    assert "lv_obj_add_flag(s_viewport_progress_bar, LV_OBJ_FLAG_HIDDEN)" in status
    assert status.index('strcmp(status->phase, "sd_cache_required") == 0') < status.index(
        'status->worker_running && status->planned_tiles > 0U'
    )
    assert '"Downloading" : "Loading"' in status
    assert '"%s %u/%u"' in status
    assert 'lv_label_set_text(s_viewport_drag_hint_label, "Drag to pan")' in status
    refresh = function_body(
        source, "bool d1l_ui_map_viewport_refresh", "static lv_obj_t *map_menu_row"
    )
    assert "s_viewport_drag_active" in refresh


def test_map_marker_overlay_is_bounded_named_passive_and_tile_independent():
    source = read("main/ui/ui_map.c")
    marker_name = function_body(
        source, "static void map_marker_display_name",
        "static int32_t map_e7_to_e6"
    )
    assert '"Mesh node"' in marker_name
    assert "marker->fingerprint" not in marker_name
    header = read("main/ui/ui_map.h")
    marker_refresh = function_body(
        source,
        "static bool map_viewport_refresh_markers",
        "static bool map_viewport_try_marker_tap",
    )
    marker_tap = function_body(
        source,
        "static bool map_viewport_try_marker_tap",
        "static bool map_viewport_consume_acquire_suppression",
    )
    drag = function_body(
        source,
        "static void map_viewport_drag_event_cb",
        "static void map_viewport_zoom_in_event_cb",
    )
    release = function_body(
        source,
        "void d1l_ui_map_viewport_release",
        "void d1l_ui_map_viewport_set_suppressed",
    )
    viewport_refresh = function_body(
        source,
        "bool d1l_ui_map_viewport_refresh",
        "bool d1l_ui_map_viewport_reacquire",
    )

    assert '#include "map/map_point_projection.h"' in source
    assert '#include "mesh/node_store.h"' in source
    assert "MAP_MARKER_QUERY_LIMIT D1L_NODE_SD_HISTORY_CAPACITY" in source
    assert "MAP_MARKER_LABEL_LIMIT 8U" in source
    assert "MAP_MARKER_HIT_RADIUS 22" in source
    assert "d1l_node_store_marker_generation()" in marker_refresh
    assert "d1l_node_store_copy_markers(" in marker_refresh
    assert "d1l_map_point_project_e6(" in marker_refresh
    assert "map_marker_placement_allowed(&bounds)" in marker_refresh
    assert "map_marker_display_name(" in marker_refresh
    assert "map_copy_bounded_text(" in marker_refresh
    assert "LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE" in marker_refresh
    assert "label_y = point.screen_y + dot_radius + MAP_MARKER_LABEL_GAP" in marker_refresh
    assert "for (size_t i = 0; i < candidate_count; ++i)" in marker_refresh
    assert "s_viewport_marker_count++" in marker_refresh
    assert (
        "s_viewport_marker_label_count < MAP_MARKER_LABEL_LIMIT"
        in marker_refresh
    )
    assert "if (!label_allowed)" in marker_refresh
    assert "d1l_map_view_service_acquire_visible" not in marker_refresh
    assert "d1l_map_view_service_acquire_frame" not in marker_refresh
    assert "memcpy(s_viewport_pixels" not in marker_refresh
    assert "lv_obj_invalidate(s_viewport_image_obj)" not in marker_refresh
    assert "lv_obj_invalidate(s_viewport_marker_layer)" in marker_refresh
    assert "MAP_COLOR_TEXT" in marker_refresh
    assert "0xF87171" not in marker_refresh
    assert "s_viewport_marker_frame_revision" not in source
    assert "status->frame_revision != s_viewport_revision" not in marker_refresh
    assert 'strcmp(type, "chat") == 0' in source
    for ambiguous_legacy_role in ('strcmp(type, "R")', 'strcmp(type, "O")',
                                  'strcmp(type, "C")', 'strcmp(type, "S")'):
        assert ambiguous_legacy_role not in source
    assert "map_viewport_refresh_markers(&status)" in source
    assert "status.frame_revision > s_viewport_revision" in viewport_refresh
    assert "frame_update_available ?" in viewport_refresh
    assert "ESP_ERR_NOT_FOUND" in viewport_refresh

    assert "MAP_MARKER_HIT_RADIUS * MAP_MARKER_HIT_RADIUS" in marker_tap
    assert "s_viewport_open_node_detail(fingerprint);" in marker_tap
    assert "map_viewport_try_marker_tap()" in drag
    assert "lv_obj_set_pos(s_viewport_marker_layer, 1 + preview_x" in drag
    assert "map_viewport_reset_marker_state();" not in release
    assert "lv_obj_" not in release
    assert "map_viewport_marker_lease_valid" in marker_refresh
    assert marker_refresh.count("map_viewport_marker_lease_valid") >= 3
    assert marker_tap.count("map_viewport_marker_lease_valid") >= 2
    render = function_body(
        source, "void d1l_ui_map_render", "static void map_render_options_root"
    )
    assert "map_viewport_reset_marker_state();" in render
    assert "d1l_ui_map_open_node_detail_cb_t" in header


def test_map_marker_node_detail_uses_advert_coordinates_and_reacquires_same_view():
    source = read("main/ui/ui_phase1.c")
    node_detail = read("main/ui/ui_node_detail.c")
    detail = function_body(
        node_detail,
        "bool d1l_ui_node_detail_render",
        "void d1l_ui_node_detail_deactivate",
    )
    open_from_map = function_body(
        source,
        "static void open_map_node_detail",
        "static void close_route_detail_event_cb",
    )
    close = function_body(
        source,
        "static void handle_node_detail_action",
        "static void show_node_detail_view",
    )

    assert '"Advert location %s, %s"' in detail
    assert '"Advert location not provided"' in detail
    assert "entry->location_valid" in detail
    assert "entry->lat_e6" in detail
    assert "entry->lon_e6" in detail
    assert "GPS" not in detail
    assert "lv_obj_set_size(controller->sheet, 480, 480);" in node_detail
    assert "lv_obj_set_pos(controller->sheet, 0, 0);" in node_detail
    assert "d1l_app_model_query_nodes(" in open_from_map
    assert "&query, s_map_node_rows, D1L_NODE_STORE_CAPACITY" in open_from_map
    assert "D1L_NODE_STORE_CAPACITY" in open_from_map
    assert "strcmp(s_map_node_rows[i].node.fingerprint, fingerprint) == 0" in open_from_map
    assert "show_node_detail_view(&s_map_node_rows[i], true);" in open_from_map
    assert "event->return_to_map" in close
    assert "d1l_ui_map_viewport_reacquire()" in close
    assert "request_content_refresh();" in close


def test_node_and_contact_messaging_are_fail_closed_by_canonical_role():
    source = read("main/ui/ui_phase1.c")
    contact_source = read("main/ui/ui_contact_sheets.c")
    node_gate = function_body(
        source, "static bool node_view_can_dm", "static bool contact_can_dm"
    )
    contact_gate = function_body(
        source, "static bool contact_can_dm", "static bool contact_can_export"
    )
    compose = function_body(
        source, "static void open_dm_compose_for_contact", "static void open_node_dm_for"
    )
    detail = function_body(
        contact_source,
        "bool d1l_ui_contact_sheets_render_detail",
        "bool d1l_ui_contact_sheets_render_options",
    )

    assert "dm_identity_for_node(view, NULL).can_open_compose" in node_gate
    assert 'strcmp(view->role, "companion")' not in node_gate
    assert "dm_identity_for_contact(entry, NULL).can_open_compose" in contact_gate
    assert 'strcmp(entry->type, "chat")' not in contact_gate
    assert "dm_identity_for_contact(entry, &selected)" in compose
    assert "if (!eligibility.can_open_compose)" in compose
    assert "show_dm_identity_reason(eligibility)" in compose
    assert "if (controller->rendered.can_dm)" in detail
    assert '"Messaging unavailable [%s]"' in detail
    assert "d1l_ui_dm_identity_reason_text(" in detail

    nodes_source = read("main/ui/ui_nodes.c")
    contact_row = function_body(
        nodes_source, "static void nodes_render_contact_row", "static void nodes_render_node_row"
    )
    assert "const bool can_dm = controller->rendered.contact_can_dm[index]" in contact_row
    assert "if (can_dm)" in contact_row

    options = function_body(
        contact_source,
        "bool d1l_ui_contact_sheets_render_options",
        "bool d1l_ui_contact_sheets_render_forget",
    )
    assert "if (controller->rendered.can_export)" in options
    assert '"Export unavailable"' in options
    assert "create_panel(sheet, 16, 280, 448, 48)" in options


def test_map_copy_and_icon_do_not_imply_onboard_gps_positioning():
    map_source = read("main/ui/ui_map.c")
    home_source = read("main/ui/ui_home.c")
    assert '"Downloading a small area around the saved map center."' in map_source
    assert '"Preparing the saved map area."' in map_source
    assert "your location" not in map_source.lower()
    assert "LV_SYMBOL_GPS" not in home_source
    assert '"Saved center and a small local map area"' in home_source


def test_ambient_mesh_updates_do_not_tear_down_an_active_map_view():
    source = read("main/ui/ui_phase1.c")
    map_timer = function_body(
        source, "static void map_viewport_timer_cb", "static void refresh_timer_cb"
    )
    map_input = function_body(
        source,
        "static d1l_ui_map_render_input_t map_render_input_from_snapshot",
        "static void remember_rendered_map_input",
    )
    refresh_timer = function_body(
        source, "static void refresh_timer_cb", "static void create_top_bar"
    )
    changed = refresh_timer.split(
        "if (content_generation_changed_from_rendered(&s_snapshot))", 1
    )[1]

    for field in (
        "snapshot->map_location_set",
        "snapshot->map_lat_e7",
        "snapshot->map_lon_e7",
        "snapshot->map_tile_zoom",
    ):
        assert field in map_input
    assert "snapshot->map_tile_cache_ready" not in map_input
    assert "snapshot->wifi_connected" not in map_input
    assert "D1L_UI_MAP_VIEWPORT_REFRESH_MS = 500U" in source
    assert "d1l_ui_navigation_active() == D1L_UI_TAB_MAP" in map_timer
    assert "map_interactive_touch_authorized()" in map_timer
    assert "(void)d1l_ui_map_viewport_refresh();" in map_timer
    assert "lv_timer_create(map_viewport_timer_cb," in source
    assert "d1l_ui_navigation_active() == D1L_UI_TAB_MAP" in refresh_timer
    assert "map_render_input_changed_from_rendered(&s_snapshot)" in refresh_timer
    assert refresh_timer.index("map_render_input_changed_from_rendered(&s_snapshot)") < (
        refresh_timer.index("content_generation_changed_from_rendered(&s_snapshot)")
    )
    assert "remember_rendered_content_generation(&s_snapshot);" in changed
    assert "request_content_refresh();" in changed
    assert changed.index("remember_rendered_content_generation(&s_snapshot);") < changed.index(
        "request_content_refresh();"
    )


def test_storage_and_wifi_transitions_refresh_non_map_ui_without_tearing_down_map():
    source = read("main/ui/ui_phase1.c")
    generation_struct = source.split("typedef struct {\n    uint32_t rx_packets;", 1)[1].split(
        "} d1l_ui_content_generation_t;", 1
    )[0]
    generation_builder = function_body(
        source,
        "static d1l_ui_content_generation_t content_generation_from_snapshot",
        "static bool content_generation_text_equal",
    )
    generation_equal = function_body(
        source,
        "static bool content_generation_equal",
        "static void remember_rendered_content_generation",
    )
    map_input = function_body(
        source,
        "static d1l_ui_map_render_input_t map_render_input_from_snapshot",
        "static void remember_rendered_map_input",
    )
    refresh_timer = function_body(
        source, "static void refresh_timer_cb", "static void create_top_bar"
    )
    changed = refresh_timer.split(
        "if (content_generation_changed_from_rendered(&s_snapshot))", 1
    )[1]

    fields = (
        "storage_sd_present",
        "storage_sd_mounted",
        "storage_sd_data_root_ready",
        "storage_sd_needs_fat32",
        "storage_setup_required",
        "storage_data_enabled",
        "storage_retained_sd_degraded",
        "map_tile_cache_ready",
        "wifi_connected",
        "wifi_connecting",
        "storage_sd_state",
        "storage_setup_action",
    )
    for field in fields:
        assert field in generation_struct
        assert f".{field} = snapshot->{field}" in generation_builder
        assert f"left->{field}" in generation_equal
        assert f"right->{field}" in generation_equal

    assert "content_generation_text_equal(left->storage_sd_state" in generation_equal
    assert "content_generation_text_equal(left->storage_setup_action" in generation_equal
    assert "node_store_backend" in generation_struct
    assert ".node_store_backend = snapshot->node_store_backend" in generation_builder
    assert "content_generation_text_equal(left->node_store_backend" in generation_equal
    assert "snapshot->map_tile_cache_ready" not in map_input
    assert "snapshot->wifi_connected" not in map_input
    map_branch = changed.split("} else {", 1)[0]
    non_map_branch = changed.split("} else {", 1)[1]
    assert "if (map_active)" in changed
    assert "remember_rendered_content_generation(&s_snapshot);" in map_branch
    assert "request_content_refresh();" in non_map_branch


def test_map_ui_releases_before_covers_and_automation_is_fail_closed():
    source = read("main/ui/ui_phase1.c")

    show_modal = function_body(source, "static void show_modal", "static void layout_content_for_active_tab")
    assert show_modal.index("d1l_ui_map_viewport_prepare_cover();") < show_modal.index(
        "d1l_ui_map_viewport_release();"
    )
    assert show_modal.index("d1l_ui_map_viewport_release();") < show_modal.index(
        "d1l_ui_modal_show(obj);"
    )

    render_active = function_body(source, "static void render_active_tab", "esp_err_t d1l_ui_phase1_request_tab")
    assert render_active.index("d1l_ui_map_viewport_release();") < render_active.index(
        "d1l_ui_screen_render("
    )

    tab_switch = function_body(source, "static void process_pending_tab_switch", "static void process_pending_content_refresh")
    assert "d1l_ui_map_viewport_release();" in tab_switch

    capture_begin = function_body(source, "esp_err_t d1l_ui_capture_begin", "esp_err_t d1l_ui_capture_chunk")
    assert "!s_capture_shadow_ready || s_capture_active" in capture_begin
    assert "d1l_ui_map_viewport_set_suppressed(true);" in capture_begin
    capture_end = function_body(source, "esp_err_t d1l_ui_capture_end", "static void request_tab_event_cb")
    assert "if (!s_capture_active)" in capture_end
    assert "d1l_ui_map_viewport_set_suppressed(false);" in capture_end

    request_tab = function_body(source, "esp_err_t d1l_ui_phase1_request_tab", "esp_err_t d1l_ui_phase1_scroll_probe")
    assert "set_map_interactive_touch_authorized(false);" in request_tab
    assert "d1l_ui_map_viewport_suppress_next_acquire();" in request_tab

    touch_tab = function_body(source, "static void request_tab_event_cb", "static void dock_event_cb")
    assert "set_map_interactive_touch_authorized(*tab == D1L_UI_TAB_MAP);" in touch_tab
    render_map = function_body(source, "static void render_map(", "static const char *packet_filter_direction")
    assert "!map_interactive_touch_authorized()" in render_map
    assert "d1l_ui_map_viewport_suppress_next_acquire();" in render_map

    home_action = function_body(source, "static void handle_home_action", "static void render_home_screen")
    home_map = home_action.split("case D1L_UI_HOME_ACTION_MAP:", 1)[1].split("break;", 1)[0]
    assert home_map.index("set_map_interactive_touch_authorized(true);") < home_map.index(
        "request_tab_switch(D1L_UI_TAB_MAP);"
    )
    close_options = function_body(
        source,
        "static void close_map_options_sheet_event_cb(lv_event_t *event)\n{",
        "static void open_map_cache_status_event_cb(lv_event_t *event)\n{",
    )
    assert close_options.index("set_map_interactive_touch_authorized(true);") < close_options.index(
        "request_tab_switch(D1L_UI_TAB_MAP);"
    )
    save_location = function_body(
        source,
        "static void map_location_save_event_cb(lv_event_t *event)\n{",
        "static void map_location_clear_event_cb(lv_event_t *event)\n{",
    )
    assert save_location.index("hide_map_location_sheet();") < save_location.index(
        "hide_map_options_sheet();"
    ) < save_location.index("set_map_interactive_touch_authorized(true);") < save_location.index(
        "request_tab_switch(D1L_UI_TAB_MAP);"
    )
    assert "request_content_refresh();" not in save_location
    assert "set_map_interactive_touch_authorized(false);" in request_tab


def test_removed_provider_and_offline_map_ui_cannot_reappear():
    ui_source = "\n".join(
        read(path)
        for path in (
            "main/ui/ui_map.c",
            "main/ui/ui_map.h",
            "main/ui/ui_phase1.c",
            "main/ui/ui_navigation.c",
            "main/ui/ui_settings.c",
            "main/ui/ui_keyboard.c",
            "main/ui/ui_home.c",
        )
    ).lower()
    for removed in (
        "offline maps",
        "tile source",
        "map_provider",
        "map_tiles_sheet",
        "map_tile_source",
        "map_remove_confirm",
        "save_map_tile_provider",
        "clear_map_tile_provider",
    ):
        assert removed not in ui_source

    for required in (
            "map options",
            "set location",
            "cache status",
            "wi-fi needed",
            "waiting for storage",
            "loading map",
        "map unavailable",
    ):
        assert required in ui_source


def test_map_options_is_a_quiet_two_row_menu():
    source = read("main/ui/ui_map.c")
    options = function_body(
        source, "static void map_render_options_root", "static void map_render_cache_status"
    )

    assert 'map_menu_row(parent, 68, "Set location"' in options
    assert 'map_menu_row(parent, 140, "Cache status"' in options
    assert 'const char *cache_status = "Storage starting";' in options
    assert 'cache_status = "SD ready";' in options
    assert 'cache_status = "Insert SD";' in options
    assert 'cache_status = "Needs FAT32";' in options
    assert options.count("map_menu_row(") == 2
    assert 'map_panel(parent, 16, 280, 448, 80)' in options
    assert (
        '"The local node area downloads in the background. Opening Map always has priority."'
        in options
    )
    assert (
        '"Built-in tiles download only while Map is open. Offline prefetch needs an authorized SD source."'
        in options
    )
    assert "background_prefetch_permitted" in options
    assert "Choose a location and review" not in options


def test_map_cache_status_is_read_only_and_reports_saved_work_honestly():
    source = read("main/ui/ui_map.c")
    saved_status = function_body(
        source, "static void map_saved_area_status", "void d1l_ui_map_render("
    )
    cache_page = function_body(
        source, "static void map_render_cache_status", "void d1l_ui_map_render_options"
    )

    assert "status->planned_tiles == 0U" in saved_status
    assert "status->worker_running" in saved_status
    assert '"Saving %u/%u"' in saved_status
    assert "!status->worker_running" in saved_status
    assert "status->rendered_tiles == status->planned_tiles" in saved_status
    assert "status->failed_tiles == 0U" in saved_status
    assert '"Local area saved"' in saved_status
    assert "status->rendered_tiles > 0U" in saved_status
    assert '"Partly saved"' in saved_status
    assert '"Open Map to save"' in saved_status

    assert "d1l_map_view_service_status(&view_status);" in cache_page
    assert "d1l_map_view_service_acquire_visible" not in cache_page
    assert "d1l_map_view_service_acquire_frame" not in cache_page
    assert '"Built-in OpenStreetMap - interactive cache only"' in cache_page
    assert "view_status.provider_configured" in cache_page
    assert "prefetch.selected_max_zoom" in cache_page
    assert "prefetch.cache_budget_mb" in cache_page
    assert '"(c) OpenStreetMap contributors"' in cache_page
    assert 'map_render_header(parent, "Cache status", "Read-only readiness", "Back"' in cache_page


def test_unsaved_map_location_uses_empty_fields_and_range_guidance():
    map_source = read("main/ui/ui_map.c")
    phase_source = read("main/ui/ui_phase1.c")
    editor = function_body(
        map_source, "void d1l_ui_map_render_location", "void d1l_ui_map_viewport_release"
    )
    snapshot_copy = function_body(
        phase_source, "static void map_location_from_snapshot", "static bool parse_coord_text_e7"
    )

    assert 'char latitude[20] = "";' in editor
    assert 'char longitude[20] = "";' in editor
    assert "if (snapshot->map_location_set)" in editor
    assert '"Range: -90 to 90", latitude' in editor
    assert '"Range: -180 to 180", longitude' in editor
    assert "43.6532" not in editor
    assert "-79.3832" not in editor
    assert "s_map_location_lat_e7 = 0;" in snapshot_copy
    assert "s_map_location_lon_e7 = 0;" in snapshot_copy
    assert "436532000L" not in phase_source
    assert "-793832000L" not in phase_source


def test_home_map_status_reports_setup_requirements_not_sd_as_saved_content():
    source = read("main/ui/ui_home_view.c")

    assert 'const char *map_status = "Ready to open";' in source
    assert 'map_status = "Set a location";' in source
    assert "map_status = map_storage_state(input);" in source
    assert 'return "SD starting";' in source
    assert 'return "Insert SD";' in source
    helper = source.split("static const char *map_storage_state", 1)[1].split(
        "void d1l_ui_home_view", 1
    )[0]
    assert helper.index('text_equals(input->storage_setup_action, "insert_card")') < helper.index(
        "d1l_ui_home_sd_state(input)"
    )
    assert helper.index("input->storage_sd_needs_fat32") < helper.index(
        "d1l_ui_home_sd_state(input)"
    )
    assert "storage_needs_attention(input)" in helper
    assert 'map_status = "Needs Wi-Fi";' in source
    assert '"Map cache ready"' not in source
    assert '"Map setup needed"' not in source
    assert source.index("if (!input->map_location_set)") < source.index(
        "else if (!input->map_tile_cache_ready)"
    ) < source.index("else if (!input->wifi_connected)")
