import re
from pathlib import Path

from tools import ui_simulator


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def static_void_body(source: str, symbol: str) -> str:
    marker = f"static void {symbol}("
    start = source.rfind(marker)
    assert start >= 0, symbol
    end = source.find("\nstatic ", start + len(marker))
    return source[start : end if end >= 0 else len(source)]


def concrete_button_dimensions(body: str, sheet: str) -> list[tuple[int, int]]:
    return [
        (int(width), int(height))
        for width, height in re.findall(
            rf"create_button\(\s*{re.escape(sheet)}\s*,\s*[^,]+,\s*-?\d+\s*,\s*-?\d+\s*,\s*(\d+)\s*,\s*(\d+)\s*,",
            body,
        )
    ]


def test_ui_references_only_enabled_lvgl_fonts():
    source = "\n".join(path.read_text(encoding="utf-8") for path in (ROOT / "main/ui").glob("*.c"))
    sdkconfig = read("sdkconfig.defaults")
    referenced = set(re.findall(r"lv_font_montserrat_(\d+)", source))
    enabled = set(re.findall(r"CONFIG_LV_FONT_MONTSERRAT_(\d+)=y", sdkconfig))
    assert referenced <= enabled


def test_app_model_exposes_bounded_ui_snapshot():
    header = read("main/app/app_model.h")
    source = read("main/app/app_model.c")
    assert "D1L_APP_SNAPSHOT_PACKET_PREVIEW 4U" in header
    assert "D1L_APP_SNAPSHOT_DM_PREVIEW 5U" in header
    assert "D1L_APP_SNAPSHOT_NODE_PREVIEW 4U" in header
    assert "D1L_APP_SNAPSHOT_CONTACT_PREVIEW D1L_CONTACT_STORE_CAPACITY" in header
    assert "D1L_APP_SNAPSHOT_ROUTE_PREVIEW 2U" in header
    assert "D1L_APP_SNAPSHOT_ROOM_PREVIEW D1L_ROOM_SERVER_PREVIEW_CAPACITY" in header
    assert "D1L_APP_SNAPSHOT_REPEATER_PREVIEW D1L_REPEATER_PREVIEW_CAPACITY" in header
    assert "D1L_HOME_MESSAGE_PREVIEW 5U" in header
    assert "D1L_HOME_REPEATER_PREVIEW 3U" in header
    assert "d1l_home_message_preview_t" in header
    assert "d1l_home_repeater_preview_t" in header
    assert "d1l_app_snapshot_t" in header
    assert "d1l_app_model_snapshot" in header
    assert "static d1l_app_snapshot_t s_snapshot" in read("main/ui/ui_phase1.c")
    assert "d1l_contact_store_copy_recent" in source
    assert "d1l_route_store_copy_recent" in source
    assert "d1l_app_model_query_nodes(&node_query" in source
    assert "d1l_packet_log_copy_recent" in source
    assert "d1l_dm_store_copy_recent" in source
    assert "d1l_app_model_copy_dm_thread" in header
    assert "d1l_app_model_copy_dm_thread_page" in header
    assert "d1l_dm_store_query_thread_page(" in source
    assert "fingerprint, out_entries, max_entries, skip_newest, query" in source
    assert "d1l_meshcore_service_status" in source
    assert "d1l_health_snapshot" in source
    assert "d1l_connectivity_status" in source
    assert "d1l_mesh_inspector_signal_summary" in source
    assert "reset_reason" in header
    assert "onboarding_complete" in header
    assert "ui_task_stack_free_words" in header
    assert "lvgl_used_pct" in header
    assert "map_page_supported" in header
    assert "map_tile_cache_ready" in header
    assert "map_tile_download_supported" in header
    assert "map_tile_render_supported" in header
    assert "map_tile_sideload_supported" in header
    assert "map_tile_cache_policy" in header
    assert "map_tile_cache_path_template" in header
    assert "map_tile_download_state" in header
    assert "map_tile_download_requires" in header
    assert "map_location_set" in header
    assert "map_tile_provider_saved" not in header
    assert "map_tile_url_template" not in header
    assert "map_tile_attribution" not in header
    assert "map_tile_zoom" in header
    assert "map_lat_e7" in header
    assert "map_lon_e7" in header
    assert "d1l_map_tile_store_sd_ready(&storage)" in source
    assert "D1L_MAP_TILE_CACHE_POLICY" in source
    assert "snapshot->map_tile_provider_policy = D1L_MAP_TILE_PROVIDER_POLICY" in source
    assert "snapshot->map_tile_provider_attribution = D1L_MAP_TILE_PROVIDER_ATTRIBUTION" in source
    assert '"sd_cache_required"' in source
    assert '"location_required"' in source
    assert '"ready"' in source
    assert "snapshot->map_location_set = settings.map_location_set" in source
    assert "d1l_app_model_save_map_tile_provider" not in header
    assert "d1l_app_model_download_center_map_tile" not in header
    assert "d1l_app_radio_profile_edit_t" in header
    assert "radio_frequency_hz" in header
    assert "radio_bandwidth_tenths_khz" in header
    assert "radio_spreading_factor" in header
    assert "radio_coding_rate" in header
    assert "radio_tx_power_dbm" in header
    assert "radio_rx_boost" in header
    assert "radio_tcxo" in header
    assert "time_available" in header
    assert "time_label[8]" in header
    assert "home_messages[D1L_HOME_MESSAGE_PREVIEW]" in header
    assert "home_repeaters[D1L_HOME_REPEATER_PREVIEW]" in header
    assert "static void copy_cstr(char *dest, size_t dest_size, const char *src)" in source
    assert "populate_home_messages(snapshot)" in source
    assert "populate_home_repeaters(snapshot)" in source
    assert "snapshot->time_available = time_status.display_time_valid" in source
    assert "time_status.display_time_approximate" in source
    assert "time_status.timezone_settings_ready" in source
    assert "time_status.timezone_label" in source
    assert "d1l_app_model_save_radio_profile" in header
    assert "d1l_app_model_default_radio_profile" in header
    assert "d1l_app_model_current_radio_profile" in header
    assert "valid_radio_edit" in source
    assert "d1l_app_model_set_map_location" in header
    assert "d1l_app_model_clear_map_location" in header
    assert "settings.map_location_set = true" in source
    assert "settings.map_location_set = false" in source


def test_phase3_shell_replaces_diagnostic_tile_home():
    source = read("main/ui/ui_phase1.c")
    chrome = read("main/ui/ui_chrome.c")
    chrome_header = read("main/ui/ui_chrome.h")
    map_source = read("main/ui/ui_map.c")
    map_header = read("main/ui/ui_map.h")
    cmake = read("main/CMakeLists.txt")
    assert "D1L_UI_TAB_HOME" in source
    assert "D1L_UI_TAB_MESSAGES" in source
    assert "D1L_UI_TAB_NODES" in source
    assert "D1L_UI_TAB_MAP" in source
    assert "D1L_UI_TAB_PACKETS" in source
    assert "D1L_UI_TAB_SETTINGS" in source
    assert "render_map" in source
    assert "create_top_bar" in source
    assert "create_dock" in source
    assert "create_sheet" in source
    assert "create_toast" in source
    assert "create_lock_overlay" in source
    assert "create_onboarding_sheet" in source
    assert "Phase 1 hardware bring-up" not in source
    assert '"ui/ui_chrome.c"' in cmake
    assert '"ui/ui_map.c"' in cmake
    assert "#include \"ui_chrome.h\"" in source
    assert "#include \"ui_map.h\"" in source
    assert "d1l_ui_map_render" in map_source
    assert "d1l_ui_map_render" in map_header
    assert "d1l_ui_chrome_layout_t" in chrome_header
    assert "d1l_ui_chrome_layout_for_screen" in source
    assert "static void restore_dock_for_active_tab(void)" in source
    assert "set_dock_hidden(!layout.dock_visible)" in source
    assert "lv_obj_set_pos(s_content, 0, layout.content_y)" in source
    assert "lv_obj_set_size(s_content, 480, layout.content_height)" in source
    assert "lv_obj_set_size(s_top_bar, 480, layout.content_y)" in source
    assert "layout.header_detail_visible ? 8 : 1" in source
    assert "set_object_hidden(s_title_label, layout.content_y == 0U)" in source
    assert "lv_obj_align(s_title_label, LV_ALIGN_TOP_LEFT, 10, 0)" in source
    assert ".content_y = 0" in chrome
    assert ".content_height = 480" in chrome
    assert ".content_scrollable = false" in chrome
    assert ".dock_visible = false" in chrome
    assert ".header_detail_visible = false" in chrome
    assert chrome.count('.title = ""') == 2
    assert ".content_y = D1L_UI_DOCKED_CONTENT_Y" in chrome
    assert ".content_height = D1L_UI_DOCKED_CONTENT_HEIGHT" in chrome
    assert "D1L_UI_DOCKED_CONTENT_Y 0U" in chrome_header
    assert "D1L_UI_DOCK_Y 432U" in chrome_header
    assert "D1L_UI_DOCKED_CONTENT_HEIGHT" in chrome_header
    assert ".content_scrollable = true" in chrome
    assert ".dock_visible = true" in chrome
    assert chrome.count(".header_detail_visible = false") == 2
    assert ".title = \"\"" in chrome
    assert "layout_content_for_active_tab();" in source


def test_home_screen_is_user_first_companion_dashboard():
    source = read("main/ui/ui_phase1.c")
    cmake = read("main/CMakeLists.txt")
    header = read("main/app/app_model.h")
    home_module = read("main/ui/ui_home.c")
    home_header = read("main/ui/ui_home.h")
    home_view = read("main/ui/ui_home_view.c")
    home_view_header = read("main/ui/ui_home_view.h")

    assert '"ui/ui_home.c"' in cmake
    assert '"ui/ui_home_view.c"' in cmake
    assert '#include "ui_home.h"' in source
    assert "d1l_ui_home_view(&input, &s_home_controller.rendered);" in source
    assert "d1l_ui_home_render(&s_home_controller, content" in source
    assert "d1l_ui_home_render" in home_module
    assert "d1l_ui_home_render" in home_header
    assert "d1l_ui_home_sd_state" in home_view_header
    assert 'strcmp(state, "mount_pending") == 0' in home_view
    assert 'return "mounting";' in home_view
    assert 'return "needs FAT32";' in home_view
    assert "return input->storage_sd_state;" not in home_view
    assert "d1l_ui_home_destination_box" in home_header
    assert "d1l_ui_home_device_box" in home_header
    assert "d1l_ui_home_status_box" in home_header
    assert "static const d1l_ui_home_box_t k_destination_boxes" in home_module
    assert "static const d1l_ui_home_box_t k_device_box" in home_module
    assert "static const d1l_ui_home_box_t k_status_boxes" in home_module
    assert "[D1L_UI_HOME_DESTINATION_MESSAGES] = {12, 52, 222, 136}" in home_module
    assert "[D1L_UI_HOME_DESTINATION_NETWORK] = {246, 52, 222, 136}" in home_module
    assert "[D1L_UI_HOME_DESTINATION_MAP] = {12, 196, 222, 136}" in home_module
    assert "[D1L_UI_HOME_DESTINATION_MORE] = {246, 196, 222, 136}" in home_module
    assert "static const d1l_ui_home_box_t k_device_box = {12, 340, 456, 128}" in home_module
    assert "render_destination_card" in home_module
    assert "render_device_status" in home_module
    assert "lv_obj_set_style_border_width(panel, 1, 0)" in home_module
    assert "home_action_event_cb" in home_module
    assert "lv_event_get_user_data(event)" in home_module
    assert "d1l_ui_home_action_binding_t" in home_header
    assert "binding->controller->action_handler" in home_module
    assert "k_action_values" not in home_module
    assert "(void *)(uintptr_t)action" not in home_module
    assert "D1L_UI_HOME_ACTION_MESSAGES" in home_header
    assert "D1L_UI_HOME_ACTION_NETWORK" in home_header
    assert "D1L_UI_HOME_ACTION_MAP" in home_header
    assert "D1L_UI_HOME_ACTION_MORE" in home_header
    assert "D1L_UI_HOME_ACTION_RADIO" in home_header
    assert "D1L_UI_HOME_ACTION_WIFI" in home_header
    assert "D1L_UI_HOME_ACTION_BLE" in home_header
    assert "D1L_UI_HOME_ACTION_STORAGE" in home_header
    assert "D1L_UI_HOME_ACTION_ATTENTION" in home_header
    assert "D1L_UI_HOME_ACTION_LOCK" in home_header
    assert "D1L_UI_HOME_LAUNCHER_" not in home_header
    assert "D1L_UI_HOME_STATUS_COUNT" in home_header
    assert "render_home_launcher_tile" not in source
    assert "render_home_status_icon" not in source
    assert "render_home_message_preview" not in source
    assert "render_home_repeater_preview" not in source
    assert "s_title_label" in source
    assert "set_object_hidden(s_status_label, !layout.header_detail_visible)" in source
    assert "set_object_hidden(s_identity_label, !layout.header_detail_visible)" in source
    assert "set_object_hidden(s_lock_button, !layout.header_detail_visible)" in source
    assert '"Messages"' in home_module
    assert '"Nodes"' in home_module
    assert '"Map"' in home_module
    assert '"Tools"' in home_module
    assert '"Mesh"' in home_module
    assert '"Wi-Fi"' in home_module
    assert '"BLE"' in home_module
    assert '"SD"' in home_module
    assert '"Attention"' in home_module
    assert "input->public_unread_count" in home_view
    assert "input->dm_unread_count" in home_view
    assert "input->contact_count" in home_view
    assert "input->node_count" in home_view
    assert "input->packet_count" in home_view
    assert "snapshot->" not in home_module
    assert "handle_home_action" in source
    for action, tab in (
        ("D1L_UI_HOME_ACTION_MESSAGES", "D1L_UI_TAB_MESSAGES"),
        ("D1L_UI_HOME_ACTION_NETWORK", "D1L_UI_TAB_NODES"),
        ("D1L_UI_HOME_ACTION_MAP", "D1L_UI_TAB_MAP"),
        ("D1L_UI_HOME_ACTION_MORE", "D1L_UI_TAB_SETTINGS"),
    ):
        route_body = source.split(f"case {action}:", 1)[1].split("break;", 1)[0]
        assert f"request_tab_switch({tab});" in route_body
    for action, callback in (
        ("D1L_UI_HOME_ACTION_RADIO", "open_radio_settings_event_cb(NULL);"),
        ("D1L_UI_HOME_ACTION_WIFI", "open_wifi_sheet_event_cb(NULL);"),
        ("D1L_UI_HOME_ACTION_BLE", "open_ble_sheet_event_cb(NULL);"),
        ("D1L_UI_HOME_ACTION_STORAGE", "open_storage_sheet_event_cb(NULL);"),
        ("D1L_UI_HOME_ACTION_ATTENTION", "open_diagnostics_sheet_event_cb(NULL);"),
        ("D1L_UI_HOME_ACTION_LOCK", "lock_event_cb(NULL);"),
    ):
        route_body = source.split(f"case {action}:", 1)[1].split("break;", 1)[0]
        assert callback in route_body
    home_body = home_module.split("void d1l_ui_home_render", 1)[1]
    assert home_body.count("render_destination_card(parent") == 4
    assert "render_device_status(parent, &controller->rendered" in home_body
    assert "render_status_item" in home_module
    assert "Mesh %s  RX %lu  TX %lu" not in home_body
    assert "Last Messages" not in home_body
    assert "Local Repeaters" not in home_body
    assert "RF Packets" not in home_body
    assert "System" not in home_body
    assert "#define D1L_APP_SNAPSHOT_MESSAGE_PREVIEW 5U" in header
    assert "#define D1L_HOME_MESSAGE_PREVIEW 5U" in header
    assert "#define D1L_HOME_REPEATER_PREVIEW 3U" in header


def test_p0_message_layouts_keep_text_out_of_headers_and_dock():
    source = read("main/ui/ui_phase1.c")
    messages_source = read("main/ui/ui_messages.c")

    message_row = messages_source.split("static void messages_render_public_row", 1)[1].split(
        "static void messages_render_dm_row", 1
    )[0]
    dm_row = messages_source.split("static void messages_render_dm_row", 1)[1].split(
        "void d1l_ui_messages_render", 1
    )[0]
    assert "messages_create_panel(body, outgoing ? 76 : 8, y, 332, 68)" in message_row
    assert "messages_create_panel(parent, 8, y, 408, 72)" in dm_row
    assert "lv_obj_set_pos(text, 8, 26)" in message_row
    assert "lv_obj_set_pos(text, 8, 28)" in dm_row
    assert '"rssi %d | snr %s | hops %u | details >"' not in message_row
    assert '"rssi %d  snr %s  hops %u"' not in dm_row

    assert "messages_create_scroll_body(parent, 18, 60, 424, 238)" in messages_source
    assert "messages_create_scroll_body(parent, 18, 68, 424, 286)" in messages_source
    assert '"Message this channel                         >"' in messages_source
    assert "18, 306, 424, 48" in messages_source
    assert "controller->rendered.public_row_count" in messages_source
    assert "controller->rendered.dm_row_count" in messages_source

    nested_body = source.split("static lv_obj_t *create_nested_page_body", 1)[1].split(
        "static lv_obj_t *create_nested_page_label", 1
    )[0]
    assert "lv_obj_set_size(body, 448, 292)" in nested_body
    assert "lv_obj_set_pos(body, 16, 60)" in nested_body
    assert "lv_obj_set_scroll_dir(body, LV_DIR_VER)" in nested_body
    assert "lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN)" in nested_body

    detail = source.split("static void render_message_detail_sheet(void)", 1)[1].split(
        "static void show_message_detail_for", 1
    )[0]
    assert 'create_button(s_message_detail_sheet, "Back", 12, 6, 72, 44' in detail
    assert 'create_nested_page_body(s_message_detail_sheet, "message detail body")' in detail
    assert 'create_nested_page_label(body, entry->text[0] ? entry->text : "-", 0xF4F7FB, true)' in detail
    assert detail.index('entry->text[0] ? entry->text : "-"') < detail.index('"Technical details"')
    assert '"Hide technical details" : "Technical details"' in detail
    assert '"Sequence  %lu  uptime %lums  direction %s"' in detail
    assert '"Path signature  %u byte%s  saved after %s"' in detail
    assert 'return "sent by radio";' in source
    assert '"Signal was not recorded for this sent message"' in detail
    assert '"Hop count was not recorded for this sent message"' in detail

    compose = source.split("static void create_compose_sheet", 1)[1].split(
        "static void create_public_history_sheet", 1
    )[0]
    assert "static lv_obj_t *s_dock" in source
    assert "set_dock_hidden(true)" in source
    assert "restore_dock_for_active_tab()" in source
    layout = source.split("static void layout_compose_sheet_controls", 1)[1].split(
        "static bool show_channel_compose_sheet", 1
    )[0]
    assert "lv_obj_set_size(s_compose_sheet, 480, 424)" in layout
    assert "lv_obj_set_pos(s_compose_sheet, 0, 56)" in layout
    assert "lv_obj_set_style_pad_all(s_compose_sheet, 0, 0)" in layout
    assert "lv_obj_set_scroll_dir(s_compose_sheet, LV_DIR_NONE)" in layout
    assert "lv_obj_scroll_to_y(s_compose_sheet, 0, LV_ANIM_OFF)" in layout
    assert "lv_obj_set_size(s_compose_keyboard, 448, 258)" in layout
    assert "lv_obj_set_align(s_compose_keyboard, LV_ALIGN_TOP_LEFT)" in layout
    assert "lv_obj_set_pos(s_compose_keyboard, 16, 158)" in layout
    assert "d1l_ui_keyboard_configure_compose(s_compose_keyboard)" in compose
    assert "layout_compose_sheet_controls()" in compose
    probe = source.split("static void open_compose_probe_on_ui_task", 1)[1].split(
        "static void fill_compose_probe_geometry", 1
    )[0]
    assert "lv_obj_update_layout(s_screen);\n    layout_compose_sheet_controls();\n    lv_obj_update_layout(s_screen);" in probe
    assert "request_full_screen_repaint()" in probe


def test_main_content_root_is_scrollable_and_serial_tab_switchable():
    source = read("main/ui/ui_phase1.c")
    contact_source = read("main/ui/ui_contact_sheets.c")
    wifi_source = read("main/ui/ui_wifi.c")
    map_source = read("main/ui/ui_map.c")
    nodes_source = read("main/ui/ui_nodes.c")
    header = read("main/ui/ui_phase1.h")
    console = read("main/comms/usb_console.c")
    cmake = read("main/CMakeLists.txt")
    screen = read("main/ui/ui_screen.c")
    keyboard = read("main/ui/ui_keyboard.c")
    keyboard_header = read("main/ui/ui_keyboard.h")

    assert "d1l_ui_screen_configure_content_root" in screen
    assert "d1l_ui_screen_configure_content_root" in read("main/ui/ui_screen.h")
    assert "static void configure_scrollable_content_root" in screen
    assert "static void configure_home_content_root" in screen
    assert "lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE)" in screen
    assert "lv_obj_set_scroll_dir(root, LV_DIR_VER)" in screen
    assert "lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_AUTO)" in screen
    assert "lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE)" in screen
    assert "lv_obj_set_scroll_dir(root, LV_DIR_NONE)" in screen
    assert "lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF)" in screen
    assert "configure_content_scroll_root" not in source
    assert "configure_content_for_active_tab" not in source
    assert "d1l_ui_screen_configure_content_root(s_content, layout.content_scrollable)" in source
    assert "layout_content_for_active_tab()" in source
    assert "lv_obj_set_pos(s_content, 0, layout.content_y)" in source
    assert "lv_obj_set_size(s_content, 480, layout.content_height)" in source
    assert "set_dock_hidden(!layout.dock_visible)" in source
    assert "lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF)" in screen
    assert "prepare_content_root(content)" in screen
    assert "dispatch_screen(screen, content, snapshot, renderers, renderer_count)" in screen
    assert "d1l_ui_screen_render(d1l_ui_navigation_active(), &s_snapshot, s_content" in source
    assert "d1l_ui_phase1_request_tab" in header
    assert "d1l_ui_scroll_probe_result_t" in header
    assert "d1l_ui_phase1_scroll_probe" in header
    assert "d1l_ui_phase1_active_tab_name" in header
    assert "cmd_ui_status" in console
    assert "cmd_ui_tab" in console
    assert "cmd_ui_scroll_probe" in console
    assert "cmd_ui_compose_probe" in console
    assert "movement_required" in header
    assert "movement_required" in console
    assert "tx_suppressed" in header
    assert "tx_suppressed" in console
    assert "send_enabled" in header
    assert "send_enabled" in console
    assert "cmd_ui_data_canary" in console
    assert "cmd_ui_capture_status" in console
    assert "cmd_ui_capture_begin" in console
    assert "cmd_ui_capture_chunk" in console
    assert "cmd_ui_capture_end" in console
    assert '"ui status"' in console
    assert "ui tab <home|messages|nodes|map|packets|settings>" in console
    assert "ui scroll-probe <home|public_messages|dm_thread|nodes|contact_detail|contact_options|contact_forget|contact_route|mesh_roles|mesh_rooms|mesh_repeaters|packets|settings|storage|storage_card|storage_data|wifi|map|map_options|map_location|map_cache>" in console
    assert "contact_probe_surfaces" in console
    assert '\\"storage_probe_surfaces\\":[\\"storage\\",\\"storage_card\\",\\"storage_data\\"]' in console
    assert '\\"map_probe_surfaces\\":[\\"map\\",\\"map_options\\",\\"map_location\\",\\"map_cache\\"]' in console
    assert '\\"map_probes_read_only\\":true' in console
    assert "ui compose-probe <public|public-long|dm|dm-long|public-search|dm-search|packet-search|contact-edit|onboarding|map-location|wifi-ssid|wifi-password>" in console
    assert '"ui/ui_keyboard.c"' in cmake
    assert '#include "ui_keyboard.h"' in source
    assert "d1l_ui_keyboard_normalize_probe_target" in source
    assert "d1l_ui_keyboard_normalize_probe_target" in keyboard_header
    assert "d1l_ui_keyboard_probe_target_is_compose" in keyboard_header
    assert "d1l_ui_keyboard_probe_requires_hidden_dock" in keyboard_header
    assert "d1l_ui_keyboard_probe_min_width" in keyboard_header
    assert "d1l_ui_keyboard_probe_min_height" in keyboard_header
    assert "d1l_ui_keyboard_configure_compose" in keyboard_header
    assert "d1l_ui_keyboard_configure_input" in keyboard_header
    assert "d1l_ui_keyboard_focus_textarea_from_event" in keyboard_header
    assert "d1l_ui_keyboard_clear_textarea" in keyboard_header
    assert "d1l_ui_keyboard_configure_compose" in keyboard
    assert "d1l_ui_keyboard_configure_input" in keyboard
    assert "d1l_ui_keyboard_focus_textarea_from_event" in keyboard
    assert "lv_event_get_code(event)" in keyboard
    assert "LV_EVENT_FOCUSED" in keyboard
    assert "LV_EVENT_CLICKED" in keyboard
    assert "lv_event_get_target(event)" in keyboard
    assert "d1l_ui_keyboard_clear_textarea" in keyboard
    assert "lv_keyboard_set_textarea(keyboard, NULL)" in keyboard
    assert "d1l_compose_kb_map_lc" in keyboard
    assert "lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER" in keyboard
    assert "lv_obj_set_style_text_font(" in keyboard
    assert "keyboard, &d1l_ui_font_symbols_14, LV_PART_ITEMS" in keyboard
    assert "lv_obj_set_size(keyboard, (lv_coord_t)width, (lv_coord_t)height)" in keyboard
    assert "lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT)" in keyboard
    assert "lv_keyboard_set_textarea(keyboard, textarea)" in keyboard
    assert "d1l_compose_kb_map_lc" not in source
    assert "static void configure_compose_keyboard" not in source
    assert 'strcmp(normalized, "public_search") == 0' in keyboard
    assert 'strcmp(normalized, "dm_search") == 0' in keyboard
    assert 'strcmp(normalized, "packet_search") == 0' in keyboard
    assert 'strcmp(normalized, "contact_edit") == 0' in keyboard
    assert 'strcmp(normalized, "map_location") == 0' in keyboard
    assert 'strcmp(normalized, "map_provider") == 0' not in keyboard
    assert 'strcmp(normalized, "wifi_password") == 0' in keyboard
    assert "d1l_ui_keyboard_probe_target_is_compose(target)" in keyboard
    assert "d1l_ui_keyboard_probe_requires_hidden_dock" in keyboard
    assert "return 400;" in keyboard
    assert "return 250;" in keyboard
    assert "d1l_ui_keyboard_probe_min_width(canonical)" in source
    assert "d1l_ui_keyboard_probe_min_height(canonical)" in source
    assert "static bool compose_probe_target_is_dm" not in source
    assert "static bool compose_probe_target_requires_hidden_dock" not in source
    assert "static int32_t compose_probe_min_keyboard_width" not in source
    assert 'strcmp(normalized, "wifi_password") == 0' not in source
    assert "d1l_ui_keyboard_configure_input(s_public_search_keyboard, s_public_search_textarea" in source
    assert "d1l_ui_keyboard_configure_input(\n        s_dm_search_keyboard, s_dm_search_textarea" in source
    assert "d1l_ui_keyboard_configure_input(s_packet_search_keyboard, s_packet_search_textarea" in source
    assert "d1l_ui_keyboard_configure_input(" in contact_source
    assert "controller->edit_keyboard, controller->edit_textarea" in contact_source
    assert "d1l_ui_keyboard_configure_input(s_onboarding_keyboard, s_onboarding_name_textarea" in source
    assert "d1l_ui_keyboard_configure_input(controls->location_keyboard" in map_source
    assert "controls->latitude_textarea" in map_source
    assert "tile_source_keyboard" not in map_source
    assert "tile_url_textarea" not in map_source
    assert "d1l_ui_keyboard_configure_input(controller->keyboard" in wifi_source
    assert "d1l_ui_keyboard_focus_textarea_from_event(keyboard, event, latitude" in source
    assert "s_map_tiles_keyboard" not in source
    assert "d1l_ui_keyboard_focus_textarea_from_event(" in wifi_source
    assert 'strcmp(target, "public_search") == 0' in source
    assert 'strcmp(target, "dm_search") == 0' in source
    assert 'strcmp(target, "packet_search") == 0' in source
    assert 'strcmp(target, "contact_edit") == 0' in source
    assert "ui data-canary <token>" in console
    assert "ui capture status" in console
    assert "ui capture begin" in console
    assert "ui capture chunk <offset> <len>" in console
    assert "ui capture end" in console
    assert "fgetc(stdin)" in console
    assert "fgets(" not in console
    assert "LINE_TOO_LONG" in console
    assert "d1l_ui_capture_status_t" in header
    assert "d1l_ui_compose_probe_result_t" in header
    assert "d1l_ui_phase1_compose_probe" in header
    assert "d1l_ui_capture_begin" in header
    assert "d1l_ui_capture_chunk" in header
    assert 'ok_begin("ui data-canary")' in console
    assert '\\"storage_required\\":false' in console
    assert '\\"public_rf_tx\\":false' in console
    assert '\\"formats_sd\\":false' in console
    assert "process_pending_scroll_probe()" in source
    assert "process_pending_compose_probe()" in source
    assert "lv_obj_get_scroll_bottom(target)" in source
    assert "lv_obj_scroll_to_y(target, LV_COORD_MAX, LV_ANIM_OFF)" in source
    assert '"Scroll Probe DM"' in source
    assert '"state %s | reason %s | error %d | revision %lu | session %llu"' in read(
        "main/ui/ui_messages.c"
    )
    assert '"Scroll proof keeps this empty-state layout validated too."' not in nodes_source


def test_d1l_ui_display_path_uses_bsp_direct_framebuffers_and_capture_shadow():
    source = read("main/ui/ui_phase1.c")
    defaults = read("sdkconfig.defaults")

    assert "CONFIG_LCD_AVOID_TEAR=y" in defaults
    assert "CONFIG_LCD_LVGL_DIRECT_MODE=y" in defaults
    assert "bsp_lcd_get_frame_buffer(&fb1, &fb2)" in source
    assert "s_disp_drv.direct_mode = 1" in source
    assert "bsp_lcd_set_cb(lcd_flush_ready_cb, &s_disp_drv)" in source
    assert "bsp_lcd_flush_is_last_register(lcd_flush_is_last_cb)" in source
    assert "bsp_lcd_direct_mode_register(lcd_direct_mode_copy_cb)" in source
    assert "lv_disp_flush_ready(drv)" in source
    assert "if (ret != ESP_OK || !s_bsp_flush_ready_registered)" in source
    assert "D1L_UI_CAPTURE_TOTAL_BYTES" in source
    assert "copy_rect_to_capture_shadow" in source
    assert "d1l_ui_capture_begin" in source

    direct_copy = source.split("static void lcd_direct_mode_copy_cb(void)", 1)[1].split(
        "#endif\n\nstatic void flush_cb", 1
    )[0]
    assert "memcpy(to, from, bytes_per_line)" in direct_copy
    assert "copy_rect_to_capture_shadow(0, y_start, D1L_UI_CAPTURE_WIDTH, y_end" in direct_copy


def test_ui_transitions_force_full_screen_repaint_for_hardware_capture():
    source = read("main/ui/ui_phase1.c")
    cmake = read("main/CMakeLists.txt")
    screen_source = read("main/ui/ui_screen.c")
    screen_header = read("main/ui/ui_screen.h")

    assert "static void request_full_screen_repaint(void)" in source
    assert "lv_obj_invalidate(s_screen)" in source
    assert '"ui/ui_screen.c"' in cmake
    assert '#include "ui_screen.h"' in source
    assert "d1l_ui_screen_renderer_t" in screen_header
    assert "d1l_ui_screen_render" in screen_header
    assert "lv_obj_clean(content)" in screen_source
    assert "lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF)" in screen_source
    assert "renderers[i].render(content, snapshot);" in screen_source
    assert "d1l_ui_screen_render_fn_t)(lv_obj_t *content" in screen_header
    assert "static void prepare_content_root" in screen_source
    assert "lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);\n}\n\nstatic bool dispatch_screen" in screen_source
    assert "static bool dispatch_screen" in screen_source
    assert "dispatch_screen(D1L_UI_SCREEN_HOME, content, snapshot" in screen_source

    render_body = source.split("static void render_active_tab(void)", 1)[1].split(
        "esp_err_t d1l_ui_phase1_request_tab", 1
    )[0]
    assert "static const d1l_ui_screen_renderer_t renderers[]" in render_body
    for mapping in (
        "{D1L_UI_TAB_HOME, render_home_screen}",
        "{D1L_UI_TAB_MESSAGES, render_messages}",
        "{D1L_UI_TAB_NODES, render_nodes}",
        "{D1L_UI_TAB_MAP, render_map}",
        "{D1L_UI_TAB_PACKETS, render_packets}",
        "{D1L_UI_TAB_SETTINGS, render_settings}",
    ):
        assert mapping in render_body
    assert "d1l_ui_screen_render(d1l_ui_navigation_active(), &s_snapshot, s_content" in render_body
    assert "d1l_ui_screen_prepare_content_root" not in render_body
    assert "d1l_ui_screen_dispatch" not in render_body
    assert "switch (d1l_ui_navigation_active())" not in render_body
    assert "lv_obj_clean(s_content)" not in render_body
    assert "render_home_screen(&s_snapshot)" not in render_body
    assert "request_full_screen_repaint();" in render_body

    renderer_signatures = (
        "render_home_screen(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)",
        "render_messages(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)",
        "render_nodes(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)",
        "render_map(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)",
        "render_packets(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)",
        "render_settings(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)",
    )
    for signature in renderer_signatures:
        body = source.split(f"static void {signature}", 1)[1].split("\nstatic ", 1)[0]
        assert "s_content" not in body, signature

    compose_hide = source.split("static void hide_compose_sheet(void)", 1)[1].split(
        "static void hide_public_history_sheet", 1
    )[0]
    assert "request_full_screen_repaint();" in compose_hide

    channel_compose = source.split("static bool show_channel_compose_sheet", 1)[1].split(
        "static void open_compose_event_cb", 1
    )[0]
    dm_compose = source.split("static void present_dm_compose_sheet", 1)[1].split(
        "static void open_dm_compose_for_contact", 1
    )[0]
    assert "request_full_screen_repaint();" in channel_compose
    assert "request_full_screen_repaint();" in dm_compose


def test_serial_tab_state_stays_pending_until_render_finishes():
    source = read("main/ui/ui_phase1.c")
    nav_source = read("main/ui/ui_navigation.c")

    assert "static portMUX_TYPE s_navigation_lock = portMUX_INITIALIZER_UNLOCKED" in nav_source
    assert "static d1l_ui_screen_t s_active_screen = D1L_UI_SCREEN_HOME" in nav_source
    assert "static d1l_ui_screen_t s_pending_screen = D1L_UI_SCREEN_HOME" in nav_source
    assert "static void request_tab_switch(d1l_ui_tab_t tab)" in source
    assert "static bool begin_pending_tab_switch(d1l_ui_tab_t *out_tab)" in source
    assert "static void finish_pending_tab_switch(d1l_ui_tab_t rendered_tab)" in source
    assert "d1l_ui_navigation_request(tab);" in source
    assert "return d1l_ui_navigation_begin_pending(out_tab);" in source
    assert "d1l_ui_navigation_finish(rendered_tab);" in source
    assert "request_tab_switch(tab)" in source
    assert "const d1l_ui_tab_t *tab = (const d1l_ui_tab_t *)lv_event_get_user_data(event);" in source
    assert "request_tab_switch(*tab);" in source
    assert "(d1l_ui_tab_t)(uintptr_t)lv_event_get_user_data(event)" not in source
    assert "request_tab_switch(D1L_UI_TAB_MESSAGES)" in source

    process_body = source.split("static void process_pending_tab_switch(void)", 1)[1].split(
        "static void lock_event_cb", 1
    )[0]
    assert "begin_pending_tab_switch(&rendered_tab)" in process_body
    assert "d1l_ui_navigation_finish" not in process_body.split("render_active_tab();", 1)[0]
    assert "render_active_tab();" in process_body
    assert "finish_pending_tab_switch(rendered_tab);" in process_body
    assert process_body.index("render_active_tab();") < process_body.index(
        "finish_pending_tab_switch(rendered_tab);"
    )
    assert "s_content_refresh_pending = false;" not in process_body

    assert "return tab_name(d1l_ui_navigation_active());" in source
    assert "return tab_name(d1l_ui_navigation_pending());" in source
    assert "return d1l_ui_navigation_switch_pending();" in source


def test_content_refreshes_are_coalesced_on_ui_task():
    source = read("main/ui/ui_phase1.c")

    assert "static portMUX_TYPE s_content_refresh_lock = portMUX_INITIALIZER_UNLOCKED" in source
    assert "static void request_content_refresh(void)" in source
    assert "static bool begin_pending_content_refresh(void)" in source
    assert "static void process_pending_content_refresh(void)" in source
    assert "s_content_refresh_pending = false;" in source

    process_body = source.split("static void process_pending_content_refresh(void)", 1)[1].split(
        "static lv_obj_t *find_scrollable_descendant", 1
    )[0]
    assert "begin_pending_content_refresh()" in process_body
    assert "d1l_ui_phase1_tab_switch_pending()" in process_body
    assert "request_content_refresh();" in process_body
    assert "render_active_tab();" in process_body

    finish_body = source.split("static void finish_pending_tab_switch", 1)[1].split(
        "static void request_content_refresh", 1
    )[0]
    assert "s_content_refresh_pending = false;" not in finish_body

    loop_body = source.split("static void ui_task(void *arg)", 1)[1].split(
        "static void touch_poll_task", 1
    )[0]
    assert loop_body.index("process_pending_tab_switch();") < loop_body.index(
        "process_pending_content_refresh();"
    )
    assert loop_body.index("process_pending_content_refresh();") < loop_body.index(
        "process_pending_scroll_probe();"
    )


def test_ui_data_canary_uses_volatile_store_paths():
    console = read("main/comms/usb_console.c")
    message_store = read("main/mesh/message_store.h")
    dm_store = read("main/mesh/dm_store.h")
    route_store = read("main/mesh/route_store.h")
    packet_log = read("main/mesh/packet_log.h")

    assert "d1l_message_store_append_public_volatile" in message_store
    assert "d1l_dm_store_append_volatile" in dm_store
    assert "d1l_route_store_upsert_observation_volatile" in route_store
    assert "d1l_packet_log_append_raw_volatile" in packet_log

    canary = console.split("static void cmd_ui_data_canary", 1)[1].split(
        "static void cmd_storage_retained_canary", 1
    )[0]
    assert "d1l_message_store_append_public_volatile" in canary
    assert "d1l_dm_store_append_volatile" in canary
    assert "d1l_route_store_upsert_observation_volatile" in canary
    assert "d1l_packet_log_append_raw_volatile" in canary
    assert '\\"retention\\":\\"volatile\\"' in canary

    for rel in [
        "main/mesh/message_store.c",
        "main/mesh/dm_store.c",
        "main/mesh/route_store.c",
        "main/mesh/packet_log.c",
    ]:
        source = read(rel)
        assert "s_volatile_entry" in source
        assert "s_volatile_valid" in source
        assert "is_volatile_ui_canary" not in source


def test_touch_callbacks_defer_content_rebuilds_instead_of_rendering_inline():
    source = read("main/ui/ui_phase1.c")
    expected_deferred_paths = [
        "d1l_ui_packets_clear_search(&s_packets_controller);\n    if (s_packet_search_textarea)",
        "hide_packet_search_sheet();\n    request_content_refresh();",
        "d1l_ui_packets_set_search(&s_packets_controller, text);\n    hide_packet_search_sheet();\n    request_content_refresh();",
    ]
    for snippet in expected_deferred_paths:
        assert snippet in source

    rename = static_void_body(source, "contact_sheets_action_handler")
    assert "set_selected_contact(&updated)" in rename
    assert "hide_contact_edit_sheet();" in rename
    assert "show_contact_options_sheet();" in rename
    assert "d1l_ui_contact_sheets_render_detail(" not in rename


def test_touch_ui_actions_route_through_app_model():
    source = read("main/ui/ui_phase1.c")
    header = read("main/app/app_model.h")
    model = read("main/app/app_model.c")
    assert "d1l_app_model_send_public_test()" not in source
    assert "d1l_app_model_send_channel_text(" in source
    assert "s_compose_channel_id, text" in source
    assert "d1l_app_model_send_dm_text(s_compose_contact.fingerprint, text)" in source
    assert "d1l_app_model_request_advert(false)" in source
    assert "d1l_app_model_request_advert(true)" in source
    assert "d1l_app_model_complete_onboarding(name)" in source
    assert "d1l_app_model_set_map_location(s_map_location_lat_e7" in source
    assert "d1l_app_model_clear_map_location()" in source
    assert "d1l_app_model_send_channel_text" in header
    assert "d1l_app_model_send_dm_text" in header
    assert "d1l_app_model_complete_onboarding" in header
    assert "d1l_app_model_find_contact" in header
    assert "d1l_app_model_set_contact_flags" in header
    assert "d1l_app_model_export_contact_uri" in header
    assert "d1l_app_model_copy_route_trace" in header
    assert 'd1l_meshcore_service_send_public("test")' not in model
    assert "d1l_meshcore_service_send_public(text)" in model
    assert "d1l_meshcore_service_send_dm(fingerprint, text)" in model
    assert "d1l_contact_store_find_by_fingerprint(fingerprint, out_contact)" in model
    assert "d1l_contact_store_set_flags(fingerprint, favorite, muted, out_contact)" in model
    assert "d1l_contact_store_export_uri(&contact, dest, dest_size)" in model
    assert "d1l_route_store_copy_for_target(fingerprint, out_entries, max_entries)" in model
    assert "d1l_meshcore_service_request_advert(flood)" in model
    assert "d1l_settings_complete_onboarding(" in model
    onboarding = model.split(
        "esp_err_t d1l_app_model_complete_onboarding", 1
    )[1].split("esp_err_t d1l_app_model_reset_onboarding", 1)[0]
    assert "const bool first_completion = !before.onboarding_complete" in onboarding
    assert "d1l_channel_store_seed_onboarding_defaults()" in onboarding
    assert "d1l_meshcore_service_start_rx_async()" in onboarding
    assert "d1l_meshcore_service_request_boot_advert(true)" in onboarding
    assert onboarding.index("d1l_meshcore_service_start_rx_async()") < onboarding.index(
        "d1l_meshcore_service_request_boot_advert(true)"
    )
    assert "d1l_settings_update_fields(" in model
    assert "d1l_meshcore_service_ensure_identity()" in model


def test_ui_simulator_flow_names_match_lvgl_handlers():
    source = (
        read("main/ui/ui_phase1.c")
        + read("main/ui/ui_first_start.c")
        + read("main/ui/ui_wifi.c")
        + read("main/ui/ui_radio_settings.c")
        + read("main/ui/ui_radio_settings.h")
        + read("main/ui/ui_contact_sheets.c")
        + read("main/ui/ui_contact_sheets.h")
    )
    actions = {
        step["action"]
        for flow in ui_simulator.EXPECTED_FLOWS
        for step in flow["steps"]
    }
    flow_names = {flow["name"] for flow in ui_simulator.EXPECTED_FLOWS}

    assert {
        "first_boot_onboarding",
        "lock_overlay_unlock",
        "home_launcher_navigation",
        "messages_hierarchy_navigation",
            "public_compose_and_send",
            "channel_select_private_history_compose_and_return_public",
        "public_history_search",
        "dm_thread_open_and_reply",
        "node_detail_inspection",
        "contact_detail_options_hierarchy",
        "contact_rename_and_forget_confirmation",
        "map_page_policy",
        "packet_filters_search_and_details",
        "mesh_roles_browser",
        "settings_radio_storage_and_advert",
    } <= flow_names

    handler_symbols = {
        "edit_node_name": "controller->primary_textarea",
        "save_first_start_name": "save_name_and_advance",
        "save_first_start_location": "save_location_and_advance",
        "first_start_skip": "handle_skip",
        "save_first_start_wifi": "save_wifi_and_advance",
        "confirm_canadian_radio": "confirm_radio_and_advance",
        "continue_first_start_storage": "render_channels",
        "complete_first_start": "finish_onboarding",
        "unlock": "unlock_event_cb",
        "open_public_compose": "open_compose_event_cb",
        "edit_public_message": "s_compose_textarea",
        "send_channel_text": "send_compose_text",
            "select_channel_2": "D1L_UI_MESSAGES_ACTION_SELECT_CHANNEL",
        "mark_channel_read": "D1L_UI_MESSAGES_ACTION_MARK_PUBLIC_READ",
        "open_channel_history": "D1L_UI_MESSAGES_ACTION_OPEN_HISTORY",
        "open_channel_search": "open_public_search_event_cb",
        "open_channel_compose": "D1L_UI_MESSAGES_ACTION_COMPOSE_PUBLIC",
        "mark_public_read": "D1L_UI_MESSAGES_ACTION_MARK_PUBLIC_READ",
        "open_messages_root": "D1L_UI_MESSAGES_ACTION_SHOW_ROOT",
        "open_messages_public": "D1L_UI_MESSAGES_ACTION_SHOW_PUBLIC",
        "open_messages_dm": "D1L_UI_MESSAGES_ACTION_SHOW_DIRECT",
        "open_public_history": "open_public_history_event_cb",
        "open_public_search": "open_public_search_event_cb",
        "open_message_detail": "D1L_UI_MESSAGES_ACTION_OPEN_PUBLIC_MESSAGE",
        "open_public_reply": "reply_message_detail_event_cb",
        "edit_public_search": "s_public_search_textarea",
        "apply_public_search": "apply_public_search_event_cb",
        "open_dm_thread": "D1L_UI_MESSAGES_ACTION_OPEN_DM_THREAD",
        "open_dm_reply": "D1L_UI_MESSAGES_ACTION_REPLY_DM_THREAD",
        "toggle_dm_details": "D1L_UI_MESSAGES_ACTION_TOGGLE_DM_DETAILS",
        "open_contact_detail": "open_contact_detail_event_cb",
        "close_contact_detail": "D1L_UI_CONTACT_ACTION_CLOSE_DETAIL",
        "open_dm_compose": "D1L_UI_CONTACT_ACTION_MESSAGE",
        "open_contact_options": "D1L_UI_CONTACT_ACTION_OPEN_OPTIONS",
        "close_contact_options": "D1L_UI_CONTACT_ACTION_CLOSE_OPTIONS",
        "open_node_detail": "D1L_UI_NODES_ACTION_OPEN_NODE",
        "close_node_detail": "D1L_UI_NODE_DETAIL_ACTION_CLOSE",
        "open_contact_edit": "D1L_UI_CONTACT_ACTION_RENAME",
        "close_contact_edit": "D1L_UI_CONTACT_ACTION_CANCEL_EDIT",
        "cancel_contact_edit": "D1L_UI_CONTACT_ACTION_CANCEL_EDIT",
        "edit_contact_alias": "edit_textarea",
        "save_contact_alias": "D1L_UI_CONTACT_ACTION_SAVE_EDIT",
        "open_forget_contact_confirm": "D1L_UI_CONTACT_ACTION_OPEN_FORGET",
        "close_forget_contact_confirm": "D1L_UI_CONTACT_ACTION_CANCEL_FORGET",
        "cancel_forget_contact": "D1L_UI_CONTACT_ACTION_CANCEL_FORGET",
        "confirm_forget_contact": "D1L_UI_CONTACT_ACTION_CONFIRM_FORGET",
        "open_map": "dock_event_cb",
        "open_nodes": "dock_event_cb",
        "open_packets": "D1L_UI_SETTINGS_ACTION_PACKETS",
        "open_settings": "dock_event_cb",
        "open_map_options": "open_map_options_sheet_event_cb",
        "close_map_options": "close_map_options_sheet_event_cb",
        "open_map_location": "open_map_location_from_options_event_cb",
        "edit_map_latitude": "d1l_ui_map_latitude_textarea",
        "edit_map_longitude": "d1l_ui_map_longitude_textarea",
        "save_map_location": "map_location_save_event_cb",
        "close_map_location": "close_map_location_sheet_event_cb",
        "open_map_cache": "open_map_cache_status_event_cb",
        "close_map_cache": "back_to_map_options_event_cb",
        "open_contact_export": "D1L_UI_CONTACT_ACTION_EXPORT",
        "close_contact_export": "D1L_UI_CONTACT_ACTION_CLOSE_EXPORT",
        "open_route_trace": "open_route_trace_event_cb",
        "close_route_trace": "close_route_trace_event_cb",
        "send_contact_trace": "route_trace_request_event_cb",
        "open_packet_search": "open_packet_search_event_cb",
        "edit_packet_search": "s_packet_search_textarea",
        "apply_packet_search": "apply_packet_search_event_cb",
        "open_packet_detail": "open_packet_detail_event_cb",
        "open_route_detail": "open_route_detail_event_cb",
        "open_mesh_roles": "open_mesh_roles_event_cb",
        "close_mesh_roles": "close_mesh_roles_event_cb",
        "open_mesh_rooms": "open_mesh_room_servers_event_cb",
        "close_mesh_rooms": "mesh_roles_subpage_back_event_cb",
        "open_mesh_repeaters": "open_mesh_repeater_candidates_event_cb",
        "close_mesh_repeaters": "mesh_roles_subpage_back_event_cb",
        "open_radio_settings": "open_radio_settings_event_cb",
        "radio_freq_down": "D1L_UI_RADIO_SETTINGS_ACTION_FREQ_DOWN",
        "radio_cycle_bandwidth": "D1L_UI_RADIO_SETTINGS_ACTION_BANDWIDTH",
        "save_radio_profile": "D1L_UI_RADIO_SETTINGS_ACTION_SAVE",
        "open_storage_setup": "open_storage_sheet_event_cb",
        "wifi_scan": "D1L_UI_WIFI_ACTION_SCAN",
        "wifi_connect": "D1L_UI_WIFI_ACTION_CONNECT",
        "open_display_settings": "open_display_sheet_event_cb",
        "open_diagnostics": "open_diagnostics_sheet_event_cb",
        "open_advert_sheet": "open_sheet_event_cb",
        "send_advert_zero": "advert_zero_event_cb",
        "send_advert_flood": "advert_flood_event_cb",
    }
    assert set(handler_symbols) <= actions
    for symbol in handler_symbols.values():
        assert symbol in source


def test_first_boot_onboarding_sheet_is_touch_backed_and_persisted():
    source = read("main/ui/ui_phase1.c")
    assert "static lv_obj_t *s_onboarding_sheet" in source
    assert "static lv_obj_t *s_onboarding_name_textarea" in source
    assert "create_onboarding_sheet" in source
    assert "update_onboarding_visibility" in source
    assert "complete_onboarding_from_ui" in source
    assert '"MeshCore DeskOS D1L"' in source
    assert '"First boot setup"' in source
    assert '"Node name"' in source
    assert "D1L_NODE_NAME_FACTORY_DEFAULT" in source
    onboarding_callbacks = source.split(
        "static void complete_onboarding_from_ui", 1
    )[1].split("static const char *public_row_state", 1)[0]
    assert '"D1L Desk"' not in onboarding_callbacks
    assert '"Canada/USA preset confirmed  910.525 BW62.5 SF7 CR5"' in source
    assert '"Role Desk Companion  Wi-Fi off  BLE off  Observer off"' in source
    assert 'create_button(s_onboarding_sheet, "Start"' in source
    assert 'create_button(s_onboarding_sheet, "Use Defaults"' in source
    assert "lv_textarea_set_max_length(s_onboarding_name_textarea, D1L_NODE_NAME_LEN - 1U)" in source
    assert "lv_keyboard_set_textarea(s_onboarding_keyboard, s_onboarding_name_textarea)" in source
    onboarding_body = source.split("static void create_onboarding_sheet", 1)[1].split(
        "static void create_lock_overlay", 1
    )[0]
    assert "lv_obj_set_size(s_onboarding_sheet, 480, 480)" in onboarding_body
    assert "lv_obj_set_pos(s_onboarding_sheet, 0, 0)" in onboarding_body
    assert "lv_obj_set_style_border_width(s_onboarding_sheet, 0, 0)" in onboarding_body
    update_body = source.split("static void update_onboarding_visibility", 1)[1].split(
        "static void update_chrome", 1
    )[0]
    assert "request_full_screen_repaint();" in update_body
    assert "snapshot->onboarding_complete" in source


def test_ui_allocation_wrappers_and_stack_budget_are_hardened():
    source = read("main/ui/ui_phase1.c")
    device_sheets = read("main/ui/ui_device_sheets.c")
    contact_sheets = read("main/ui/ui_contact_sheets.c")
    assert "#define D1L_UI_TASK_STACK_BYTES 12288U" in source
    assert "#define D1L_TOUCH_TASK_STACK_BYTES 4096U" in source
    assert '#include "freertos/idf_additions.h"' in source
    assert "xTaskCreatePinnedToCoreWithCaps(" in source
    assert 'ui_task, "d1l_ui", D1L_UI_TASK_STACK_BYTES' in source
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in source
    assert ") != pdPASS" in source
    assert "s_ui_task_handle = NULL" in source
    assert "s_touch_task_handle = NULL" in source
    assert "bool d1l_ui_phase1_started(void)" in source
    assert "ui_task_stack_free_words" in device_sheets
    assert "create_screen_object" in source
    assert "create_object" in source
    assert "create_label_object" in source
    assert "create_textarea" in source
    assert "create_keyboard" in source
    assert "lv_qrcode_create" in contact_sheets
    assert 's_screen = create_screen_object("root screen")' in source
    assert 's_content = create_object(s_screen, "content root")' in source
    assert 's_compose_textarea = create_textarea(s_compose_sheet, "compose textarea")' in source
    assert 's_compose_keyboard = create_keyboard(s_compose_sheet, "compose keyboard")' in source
    assert "controller->export_sheet = create_sheet(parent);" in contact_sheets


def test_public_composer_uses_lvgl_textarea_keyboard():
    source = read("main/ui/ui_phase1.c")
    messages_source = read("main/ui/ui_messages.c")
    assert "create_compose_sheet" in source
    assert "open_compose_event_cb" in source
    assert "show_channel_compose_sheet" in source
    assert "send_compose_text" in source
    assert "static lv_obj_t *s_compose_counter" in source
    assert "update_compose_counter" in source
    assert 'label_set_fmt(s_compose_counter, "%u chars | %u/%u B"' in source
    assert "d1l_user_text_validate(text)" in source
    assert "static lv_obj_t *s_compose_send_button" in source
    assert "lv_obj_add_state(s_compose_send_button, LV_STATE_DISABLED)" in source
    assert "lv_obj_clear_state(s_compose_send_button, LV_STATE_DISABLED)" in source
    send = source.split("static void send_compose_text(void)", 1)[1].split(
        "static void send_compose_event_cb", 1
    )[0]
    validation = send.index("d1l_user_text_validate(text)")
    assert validation < send.index("d1l_app_model_send_dm_text")
    assert validation < send.index("d1l_app_model_send_channel_text")
    admission = send.index("compose_eligibility_for_text(&info)")
    assert validation < admission < send.index("d1l_app_model_send_dm_text")
    assert send.index("d1l_app_model_snapshot(&s_snapshot)") < admission
    rejected = send.split("if (!eligibility.send_enabled)", 1)[1].split(
        "esp_err_t ret", 1
    )[0]
    assert "return;" in rejected
    assert "s_compose_last_send_error = ret" in send
    assert "show_toast_text(retry.status, false)" in send
    assert "lv_textarea_set_text(s_compose_textarea, \"\")" in send
    assert "D1L_MESSAGE_MAX_CHARS" in source
    assert "lv_textarea_create" in source
    assert "lv_textarea_set_max_length" in source
    assert "LV_EVENT_VALUE_CHANGED" in source
    assert "compose_textarea_event_cb" in source
    assert "lv_textarea_get_text" in source
    assert "lv_keyboard_create" in source
    assert "lv_keyboard_set_textarea" in source
    keyboard = read("main/ui/ui_keyboard.c")
    assert "d1l_ui_keyboard_configure_compose(s_compose_keyboard)" in source
    assert "lv_keyboard_set_map" in keyboard
    assert "d1l_compose_kb_map_lc" in keyboard
    assert "LV_EVENT_READY" in source
    assert "LV_EVENT_CANCEL" in source
    assert "hide_compose_sheet()" in source
    assert '"Test"' not in messages_source


def test_dm_composer_opens_from_contact_rows():
    source = read("main/ui/ui_phase1.c")
    nodes_source = read("main/ui/ui_nodes.c")
    assert "static bool s_compose_dm" in source
    assert "static d1l_contact_entry_t s_compose_contact" in source
    assert "D1L_UI_NODES_ACTION_OPEN_CONTACT_DM" in source
    assert 'nodes_create_button(row, "Chat"' in nodes_source
    assert "nodes_dispatch_contact_dm_event_cb" in nodes_source
    assert "open_dm_compose_for_contact(event->contact);" in source
    assert "selected = *entry" in source
    assert "dm_identity_for_contact(entry, &selected)" in source
    assert "if (!eligibility.can_open_compose)" in source
    assert "s_compose_contact = *selected" in source
    assert 'lv_label_set_text(s_compose_title, title)' in source
    assert 'lv_textarea_set_placeholder_text(s_compose_textarea, "Direct message")' in source
    assert 'show_toast(s_compose_dm ? "DM" : "Channel message", ret)' in source


def test_dm_thread_sheet_opens_from_recent_dm_rows():
    source = read("main/ui/ui_phase1.c")
    messages_source = read("main/ui/ui_messages.c")
    messages_header = read("main/ui/ui_messages.h")
    render = source.split("static bool render_dm_thread_sheet(void)", 1)[1].split(
        "static void show_dm_thread_for", 1
    )[0]
    show = source.split("static void show_dm_thread_for", 1)[1].split(
        "static void open_home_dm_preview_event_cb", 1
    )[0]

    assert "static lv_obj_t *s_dm_thread_sheet" not in source
    assert "static char s_dm_thread_fingerprint" not in source
    assert "static d1l_dm_entry_t s_dm_thread_entries" not in source
    assert "static bool s_dm_thread_unread" not in source
    assert "static size_t s_dm_thread_limit" not in source
    assert "lv_obj_t *thread_sheet" in messages_header
    assert "D1L_UI_MESSAGES_THREAD_MAX_ROWS (D1L_DM_STORE_CAPACITY + 1U)" in messages_header
    assert "d1l_dm_entry_t thread_entries[D1L_UI_MESSAGES_THREAD_MAX_ROWS]" in messages_header
    assert "bool thread_unread[D1L_UI_MESSAGES_THREAD_MAX_ROWS]" in messages_header
    assert "D1L_UI_MESSAGES_THREAD_INITIAL_ROWS 5U" in messages_header
    assert "D1L_UI_MESSAGES_THREAD_LOAD_OLDER_STEP 5U" in messages_header
    assert "d1l_ui_messages_create" in source
    assert "render_dm_thread_sheet" in source
    assert "D1L_UI_MESSAGES_ACTION_OPEN_DM_THREAD" in messages_source
    assert "D1L_UI_MESSAGES_ACTION_REPLY_DM_THREAD" in messages_source
    assert "D1L_UI_MESSAGES_ACTION_TOGGLE_DM_DETAILS" in messages_source
    assert "lv_obj_set_size(sheet, 480, 480)" in messages_source
    assert "lv_obj_set_pos(sheet, 0, 0)" in messages_source
    assert 'sheet, "Back", 12, 6, 72, 44' in messages_source
    assert "loader(" in messages_source
    assert "d1l_app_model_query_dm_thread_page(" in source
    assert "controller->thread_limit > D1L_UI_MESSAGES_THREAD_MAX_ROWS" in messages_source
    assert 'body, "Load Older", 0, 0, 424, 48' in messages_source
    assert "lv_obj_scroll_to_y(body, LV_COORD_MAX, LV_ANIM_OFF)" in messages_source
    assert "outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START" in messages_source
    assert "LV_LABEL_LONG_WRAP" in messages_source
    assert "entry->text[0] ? entry->text : \"-\"" in messages_source
    for state in (
        "QUEUED",
        "WAITING_RADIO",
        "TX_ACTIVE",
        "TX_DONE",
        "AWAITING_ACK",
        "ACKNOWLEDGED",
        "RETRY_WAIT",
        "RETRY_TX",
        "FAILED_RADIO",
        "FAILED_TIMEOUT",
        "FAILED_QUEUE",
        "INTERRUPTED_BY_REBOOT",
        "CANCELLED",
    ):
        assert f"D1L_DM_DELIVERY_{state}" in messages_source
    assert "entry->delivery_state" in messages_source
    assert "entry->delivery_reason" in messages_source
    assert "entry->delivery_last_error" in messages_source
    assert "entry->delivery_retry_count" in messages_source
    assert "entry->delivery_revision" in messages_source
    assert "entry->delivery_session_id" in messages_source
    assert "entry->path_hash_bytes" in messages_source
    assert "entry->path_hops" in messages_source
    assert "binding->generation != binding->controller->generation" in messages_source
    assert "expanded_delivery_session_id" in messages_source
    assert "expanded_row_seq" in messages_source
    assert "lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE)" in messages_source
    assert ".action = D1L_UI_MESSAGES_ACTION_OPEN_DM_THREAD" in messages_source
    assert "show_dm_thread_for(event->dm_message->contact_fingerprint" in source
    assert "d1l_ui_messages_thread_fingerprint(" in source
    assert "d1l_app_model_find_contact(fingerprint, &contact)" in source
    assert "open_dm_compose_for_contact(&contact)" in source
    reply = re.search(
        r'messages_create_button\(\s*sheet,\s*"Message this contact\s+>",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)',
        messages_source,
    )
    assert reply is not None
    reply_x, _, reply_width, reply_height = (int(value) for value in reply.groups())
    assert reply_x == 16
    assert reply_width == 448
    assert reply_height >= 48
    assert 'messages_create_button(\n        sheet, "Read"' not in messages_source
    assert "read_dm_thread_event_cb" not in source
    assert "d1l_app_model_mark_dm_thread_read(fingerprint)" in show
    assert show.index("d1l_app_model_mark_dm_thread_read(fingerprint)") < show.index(
        "render_dm_thread_sheet()"
    )
    assert "snapshot->dm_content_revision" in source
    assert "refresh_dm_thread" in source
    assert "hide_dm_thread_sheet()" in source


def test_public_message_detail_sheet_opens_from_public_rows():
    source = read("main/ui/ui_phase1.c")
    messages_source = read("main/ui/ui_messages.c")
    create = source.split("static void create_message_detail_sheet", 1)[1].split(
        "static void create_public_search_sheet", 1
    )[0]
    render = source.split("static void render_message_detail_sheet(void)", 1)[1].split(
        "static void show_message_detail_for", 1
    )[0]

    assert "static lv_obj_t *s_message_detail_sheet" in source
    assert "static d1l_message_entry_t s_message_detail_message" in source
    assert "render_message_detail_sheet" in source
    assert "show_message_detail_for" in source
    assert "close_message_detail_event_cb" in source
    assert "reply_message_detail_event_cb" in source
    assert "create_message_detail_sheet" in source
    assert ".action = D1L_UI_MESSAGES_ACTION_OPEN_PUBLIC_MESSAGE" in messages_source
    assert "show_message_detail_for(event->public_message);" in source
    assert "lv_obj_set_size(s_message_detail_sheet, 480, 424)" in create
    assert "lv_obj_set_pos(s_message_detail_sheet, 0, 56)" in create
    assert 'create_button(s_message_detail_sheet, "Back", 12, 6, 72, 44' in render
    assert 'create_nested_page_body(s_message_detail_sheet, "message detail body")' in render
    assert 'create_nested_page_label(body, entry->text[0] ? entry->text : "-", 0xF4F7FB, true)' in render
    assert render.index('entry->text[0] ? entry->text : "-"') < render.index(
        '"Technical details"'
    )
    reply = re.search(
        r'create_button\(s_message_detail_sheet,\s*"Reply",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)',
        render,
    )
    assert reply is not None
    reply_x, _, reply_width, reply_height = (int(value) for value in reply.groups())
    assert reply_x == 16
    assert reply_width == 216
    assert reply_height >= 48
    assert 'create_button(s_message_detail_sheet, "DM sender", 248, 360, 216, 52' in render
    assert "explain_public_sender_dm_event_cb" in source
    assert "D1L_UI_DM_IDENTITY_SOURCE_PUBLIC_SENDER_LABEL" in source
    assert 'snprintf(title, sizeof(title), "Reply %.32s"' in source
    assert 'snprintf(placeholder, sizeof(placeholder), "Reply to %.48s"' in source
    assert "show_channel_compose_sheet(" in source
    assert "entry.channel_id, title, placeholder" in source
    assert '"Signal  rssi %d  snr %s%d.%d"' in render
    assert '"Signal was not recorded for this sent message"' in render
    assert '"Path  %u hop%s"' in render
    assert '"Hop count was not recorded for this sent message"' in render
    assert '"Hide technical details" : "Technical details"' in render
    assert '"Sequence  %lu  uptime %lums  direction %s"' in source
    assert "message_delivery_label" in source
    assert "hide_message_detail_sheet()" in source


def test_nodes_screen_renders_heard_node_rows():
    source = read("main/ui/ui_phase1.c")
    nodes_header = read("main/ui/ui_nodes.h")
    nodes_source = read("main/ui/ui_nodes.c")
    node_detail_source = read("main/ui/ui_node_detail.c")
    header = read("main/app/app_model.h")
    model = read("main/app/app_model.c")
    assert "nodes_render_node_row" in nodes_source
    assert "nodes_render_contact_row" in nodes_source
    assert "d1l_ui_nodes_controller_t s_nodes_controller" in source
    assert "d1l_node_view_t node_rows[D1L_NODE_STORE_CAPACITY]" in nodes_header
    assert "recent_node_count" in header
    assert "recent_nodes" in header
    assert "d1l_node_view_t recent_nodes" in header
    assert "d1l_app_model_query_nodes" in header
    assert "d1l_node_store_query(query, out_entries, max_entries)" in model
    assert "d1l_node_query_t node_query" in model
    assert "nodes_role_badge_text" in nodes_source
    assert "nodes_role_color" in nodes_source
    assert "node_view_can_dm" in source
    assert "role_is_managed_service" in node_detail_source
    assert 'nodes_create_button(row, "Chat", 340, 7, 84, 44' in nodes_source
    assert "lv_obj_set_style_pad_all(panel, 0, 0);" in nodes_source
    assert "render_role_badge" in node_detail_source
    assert "nodes_dispatch_node_open_event_cb" in nodes_source
    assert "recent_contact_count" in source
    assert "recent_contacts" in source
    assert "No contacts yet" in nodes_source
    assert "No other nearby nodes" in nodes_source
    assert "node_total_written" in source
    assert "contact_total_written" in source
    assert "#define D1L_APP_SNAPSHOT_NODE_PREVIEW 4U" in header
    assert "#define D1L_APP_SNAPSHOT_CONTACT_PREVIEW D1L_CONTACT_STORE_CAPACITY" in header
    assert "i < controller->rendered.contact_row_count" in nodes_source
    assert "d1l_app_model_query_nodes(" in source
    assert "D1L_NODE_STORE_CAPACITY" in source
    assert '"Saved contacts"' in nodes_source
    assert '"Nearby"' in nodes_source
    assert "nodes_node_matches_contact" in nodes_source
    assert "i < controller->rendered.node_row_count" in nodes_source


def test_node_detail_sheet_opens_from_heard_node_rows():
    source = read("main/ui/ui_phase1.c")
    detail = read("main/ui/ui_node_detail.c")
    header = read("main/ui/ui_node_detail.h")
    assert "d1l_ui_node_detail_controller_t" in header
    assert "s_node_detail_controller EXT_RAM_BSS_ATTR" in source
    assert "d1l_ui_node_detail_create" in source
    assert "d1l_ui_node_detail_render" in source
    assert "D1L_UI_NODES_ACTION_OPEN_NODE" in source
    assert "show_node_detail_view(event->node, false);" in source
    assert "D1L_UI_NODE_DETAIL_ACTION_CLOSE" in source
    assert "hide_node_detail_sheet()" in source
    assert 'create_label(controller->sheet, "Contact"' in detail
    assert '"Role %s"' in detail
    assert '"%s route  |  %u hop%s  |  %s"' in detail
    assert '"Last signal %d dBm  |  SNR %s%d.%d"' in detail
    assert '"Heard on this boot  |  %lu sighting%s"' in detail
    assert '"Advanced identity  %.16s  |  key %s"' in detail
    assert 'create_button(controller, "DM"' in detail
    assert '"Login"' in detail
    assert '"Sign in to open repeater or room controls."' in detail
    assert 'create_button(controller, "Close"' in detail
    assert "d1l_ui_node_detail_create(&s_node_detail_controller, s_screen)" in source


def test_map_screen_uses_built_in_source_and_a_bounded_visible_view():
    source = read("main/ui/ui_phase1.c")
    map_source = read("main/ui/ui_map.c")
    map_header = read("main/ui/ui_map.h")
    map_service_header = read("main/map/map_view_service.h")
    cmake = read("main/CMakeLists.txt")
    header = read("main/app/app_model.h")
    model = read("main/app/app_model.c")

    assert "D1L_UI_TAB_MAP" in source
    assert '"ui/ui_map.c"' in cmake
    assert '#include "ui_map.h"' in source
    for api in (
        "d1l_ui_map_render",
        "d1l_ui_map_render_options",
        "d1l_ui_map_render_location",
        "d1l_ui_map_viewport_release",
        "d1l_ui_map_viewport_set_suppressed",
        "d1l_ui_map_viewport_refresh",
    ):
        assert api in map_header
        assert api in map_source

    wrapper = source.split(
        "static void render_map(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)", 1
    )[1].split("\n}", 1)[0]
    assert "d1l_ui_map_render(content, snapshot, &callbacks);" in wrapper
    assert "d1l_ui_map_viewport_suppress_next_acquire();" in wrapper
    assert "map_panel(" not in wrapper
    assert "map_button(" not in wrapper
    assert "render_metric_card(" not in wrapper

    for label in (
        '"Options"',
        '"Map options"',
        '"Set location"',
        '"Cache status"',
        '"Save location"',
        '"Clear location"',
        '"Wi-Fi"',
        '"SD card"',
        '"Map view"',
        '"(c) OpenStreetMap contributors"',
    ):
        assert label in map_source

    for removed_surface in (
        '"Offline Maps"',
        '"Offline maps"',
        '"Tile source"',
        '"Save source"',
        '"Remove tile source?"',
        '"Remove source"',
    ):
        assert removed_surface not in map_source

    landing = map_source.split("void d1l_ui_map_render(", 1)[1].split(
        "static void map_render_options_root", 1
    )[0]
    assert 'map_label(parent, "Map"' not in landing
    assert landing.count('viewport, "Options", 8, 8, 96, 48') == 1
    assert "map_view_service_acquire_visible" in landing
    assert "s_viewport_lat_e7, s_viewport_lon_e7, s_viewport_zoom" in landing
    assert (
        'viewport, "Center", 8, (int)MAP_VIEWPORT_HEIGHT - 60, 96, 52'
        in landing
    )
    assert 'viewport, "-", 420, 92, 52, 52' in landing
    assert 'viewport, "+", 420, 8, 52, 52' in landing
    assert 'map_label(viewport, "Drag to pan"' in landing
    assert "MAP_VIEWPORT_WIDTH 478U" in map_source
    assert "MAP_VIEWPORT_HEIGHT (D1L_UI_DOCKED_CONTENT_HEIGHT - 2U)" in map_source
    assert "MAP_VIEWPORT_PANEL_HEIGHT (MAP_VIEWPORT_HEIGHT + 2U)" in map_source
    assert "parent, 0, 0, 480, MAP_VIEWPORT_PANEL_HEIGHT" in landing
    assert "(int)MAP_VIEWPORT_HEIGHT - 36" in landing

    options = map_source.split("static void map_render_options_root", 1)[1].split(
        "static void map_render_cache_status", 1
    )[0]
    assert 'map_menu_row(parent, 68, "Set location"' in options
    assert 'map_menu_row(parent, 140, "Cache status"' in options
    assert 'const char *cache_status = "Storage starting";' in options
    assert 'cache_status = "SD ready";' in options
    assert 'cache_status = "Insert SD";' in options
    assert 'cache_status = "Needs FAT32";' in options
    assert options.count("map_menu_row(") == 2
    assert '"Built-in map"' not in options
    assert "prefetch.provider_configured" in options
    assert "prefetch.background_prefetch_permitted" in options
    assert (
        '"The local node area downloads in the background. Opening Map always has priority."'
        in options
    )
    assert (
        '"Built-in tiles download only while Map is open. Offline prefetch needs an authorized SD source."'
        in options
    )
    assert "D1L_UI_MAP_OPTIONS_ROOT" in map_header
    assert "D1L_UI_MAP_OPTIONS_CACHE_STATUS" in map_header
    assert "D1L_UI_MAP_TILE_SOURCE" not in map_header
    assert "D1L_UI_MAP_REMOVE_CONFIRM" not in map_header

    concrete_map_buttons = [
        (int(width), int(height))
        for width, height in re.findall(
            r'map_button\(\s*[^,]+,\s*(?:"[^"]+"|back_label),\s*'
            r'-?\d+,\s*-?\d+,\s*(\d+),\s*(\d+),',
            map_source,
            re.S,
        )
    ]
    assert concrete_map_buttons
    assert all(width >= 44 and height >= 44 for width, height in concrete_map_buttons)
    for control in ("latitude_textarea", "longitude_textarea"):
        match = re.search(
            rf"lv_obj_set_size\(controls->{control},\s*(\d+),\s*(\d+)\)",
            map_source,
        )
        assert match, control
        assert int(match.group(1)) >= 44 and int(match.group(2)) >= 44, control

    assert "s_map_location_prompt_seen" not in source
    assert "d1l_app_model_download_center_map_tile" not in source
    assert "d1l_app_model_download_center_map_tile" not in map_source
    assert "download_center" not in map_header
    assert "d1l_app_model_" not in map_source
    assert "d1l_meshcore_" not in map_source
    assert "d1l_connectivity_" not in map_source
    assert "d1l_storage_" not in map_source

    assert "snapshot->map_tile_cache_ready" in map_source
    assert "snapshot->map_location_set" in map_source
    assert "d1l_ui_map_sheets_create(&s_map_sheets_controller, s_screen)" in source
    assert "open_map_location_sheet_event_cb" in source
    assert "open_map_options_sheet_event_cb" in source
    assert "render_map_options_sheet" in source
    assert "d1l_ui_map_sheets_render_options" in source
    assert "open_map_tiles_sheet_event_cb" not in source
    assert "create_map_tiles_sheet" not in source
    assert "map_provider" not in source
    assert "map_location_textarea_event_cb" in source
    assert "map_location_keyboard_event_cb" in source
    assert "parse_coord_text_e7" in source
    assert "map_location_save_event_cb" in source
    assert "map_format_coordinate" in map_source
    assert '{D1L_UI_TAB_HOME, "Home", LV_SYMBOL_HOME}' in source
    assert '{D1L_UI_TAB_MESSAGES, "Channels", LV_SYMBOL_ENVELOPE}' in source
    assert '{D1L_UI_TAB_NODES, "Contacts", LV_SYMBOL_LIST}' in source
    assert '{D1L_UI_TAB_MAP, "Map", LV_SYMBOL_IMAGE}' in source
    assert '{D1L_UI_TAB_SETTINGS, "Settings", LV_SYMBOL_SETTINGS}' in source
    assert "create_dock_button" in source
    assert "lv_obj_set_size(button, 88, 44)" in source
    assert "lv_obj_add_state(s_dock_buttons[i], LV_STATE_CHECKED)" in source
    assert "lv_obj_clear_state(s_dock_buttons[i], LV_STATE_CHECKED)" in source
    assert "D1L_UI_TAB_PACKETS" not in source.split("static void create_dock", 1)[1].split(
        "static void create_toast", 1
    )[0]
    assert "4 + (int)i * 96" in source
    assert "(void *)&item->tab" in source
    assert "(void *)(uintptr_t)i" not in source
    assert "map_tile_download_supported" in header
    assert "map_tile_render_supported" in header
    assert "map_tile_sideload_supported" in header
    assert "map_tile_provider_policy" in header
    assert "map_tile_provider_attribution" in header
    assert "D1L_MAP_TILE_RENDER_SUPPORTED true" in header
    assert "connectivity.wifi_connected" in model
    assert "snapshot->map_tile_render_supported" in model
    assert '"tile_render_pending"' in model
    assert '"sd_cache_required"' in model
    assert '"wifi_required"' in model
    assert "snapshot->map_tile_sideload_supported = false" in model
    assert "snapshot->map_tile_provider_policy = D1L_MAP_TILE_PROVIDER_POLICY" in model
    assert "snapshot->map_tile_provider_attribution = D1L_MAP_TILE_PROVIDER_ATTRIBUTION" in model
    for api in (
        "d1l_map_view_service_acquire_visible",
        "d1l_map_view_service_release_visible",
        "d1l_map_view_service_acquire_frame",
        "d1l_map_view_service_release_frame",
    ):
        assert api in map_service_header


def test_messages_screen_renders_bounded_preview_rows():
    source = read("main/ui/ui_phase1.c")
    messages_source = read("main/ui/ui_messages.c")
    messages_header = read("main/ui/ui_messages.h")
    header = read("main/app/app_model.h")
    assert "#define D1L_APP_SNAPSHOT_MESSAGE_PREVIEW 5U" in header
    assert "#define D1L_APP_SNAPSHOT_DM_PREVIEW 5U" in header
    assert "static d1l_ui_messages_mode_t s_messages_mode" in source
    assert "set_messages_mode(D1L_UI_MESSAGES_MODE_ROOT)" in source
    assert "set_messages_mode(D1L_UI_MESSAGES_MODE_PUBLIC)" in source
    assert "set_messages_mode(D1L_UI_MESSAGES_MODE_DIRECT)" in source
    assert '"Group conversations"' in messages_source
    assert "messages_render_channel_row(" in messages_source
    assert '"Direct messages"' in messages_source
    assert "for (size_t i = 0; i < controller->rendered.public_row_count; ++i)" in messages_source
    assert "for (size_t i = 0; i < controller->rendered.dm_row_count; ++i)" in messages_source
    assert "row_y + (int)i * 74" in messages_source
    assert "row_y + (int)i * 80" in messages_source
    assert "public_rows[D1L_UI_MESSAGES_PUBLIC_PREVIEW_ROWS]" in messages_header
    assert "dm_rows[D1L_UI_MESSAGES_DM_PREVIEW_ROWS]" in messages_header
    assert "snapshot->message_count" in source
    assert "snapshot->dm_conversation_count" in source
    assert "snapshot->public_unread_count" in source
    assert "snapshot->dm_unread_count" in source
    assert "snapshot->muted_dm_unread_count" in source
    assert "snapshot->recent_dm_unread_count" in source
    assert "snapshot->recent_dm_muted" in source
    assert "D1L_UI_MESSAGES_ACTION_OPEN_HISTORY" in messages_source
    assert "static d1l_message_entry_t s_public_history_entries[D1L_MESSAGE_STORE_CAPACITY]" in source
    assert "static size_t s_public_history_limit" in source
    assert "render_public_history_sheet" in source
    assert "public_history_load_older_event_cb" in source
    assert "d1l_app_model_query_channel_messages_page(" in source
    assert "s_public_history_channel_id, s_public_history_entries" in source
    assert "D1L_PUBLIC_HISTORY_UI_LOAD_OLDER_STEP" in source
    assert "lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF)" in source
    assert 'create_button(s_public_history_sheet, "Load Older"' in source
    assert 'public_history ? "Public History" : "History"' in source
    assert '"%.24s | Showing %u of %u saved"' in source
    assert '"Public Search"' in source
    assert '"Search author or message"' in source


def test_contact_pages_enforce_progressive_disclosure_and_safe_removal():
    source = read("main/ui/ui_phase1.c")
    contact_source = read("main/ui/ui_contact_sheets.c")
    contact_header = read("main/ui/ui_contact_sheets.h")
    nodes_source = read("main/ui/ui_nodes.c")

    for raw_owner in (
        "static lv_obj_t *s_contact_detail_sheet",
        "static lv_obj_t *s_contact_options_sheet",
        "static lv_obj_t *s_contact_forget_sheet",
        "static lv_obj_t *s_contact_edit_sheet",
        "static lv_obj_t *s_contact_export_sheet",
        "static d1l_contact_entry_t s_contact_detail_contact",
        "static d1l_contact_entry_t s_contact_export_contact",
    ):
        assert raw_owner not in source
    assert "s_contact_sheets_controller EXT_RAM_BSS_ATTR" in source
    assert "static lv_obj_t *s_route_trace_sheet" in source
    assert "static d1l_contact_entry_t s_route_trace_contact" in source
    assert "static d1l_route_entry_t s_route_trace_entries[D1L_ROUTE_STORE_CAPACITY]" in source
    assert "open_contact_detail_event_cb" in source
    assert "contact_sheets_action_handler" in source
    assert "open_route_trace_event_cb" in source
    for legacy in (
        "create_contact_detail_sheet",
        "create_contact_options_sheet",
        "create_contact_forget_sheet",
        "create_contact_edit_sheet",
        "create_contact_export_sheet",
        "render_contact_detail_sheet",
        "render_contact_options_sheet",
        "render_contact_forget_sheet",
        "render_contact_export_sheet",
    ):
        assert legacy not in source
    assert "d1l_ui_contact_sheets_create(" in source
    for renderer in ("detail", "options", "forget", "export", "edit"):
        assert f"d1l_ui_contact_sheets_render_{renderer}(" in source
    assert "create_route_trace_sheet" in source
    assert "render_route_trace_sheet" in source
    assert "nodes_dispatch_contact_open_event_cb" in nodes_source
    assert "show_contact_detail_for(event->contact);" in source
    assert "d1l_app_model_set_contact_flags(" in source
    assert "d1l_app_model_rename_contact(" in source
    assert "d1l_app_model_export_contact_uri(" in source
    assert "D1L_UI_CONTACT_ACTION_TOGGLE_FAVORITE" in contact_header
    assert "D1L_UI_CONTACT_ACTION_TOGGLE_MUTE" in contact_header
    assert "open_dm_compose_for_contact(contact)" in source
    assert "d1l_app_model_copy_route_trace(contact->fingerprint, s_route_trace_entries" in source
    assert "route_trace_request_event_cb" in source
    assert "d1l_app_model_send_trace_contact(" in source
    assert 'create_button(s_route_trace_sheet, "Trace"' in source
    assert '"Only the name changes; messages stay saved"' in contact_source
    assert '"Probe and TRACE are contact-only RF; Reset forgets only this contact; no Public RF"' in source
    assert '"Authenticated TRACE' not in source
    assert '"Contact Export"' in contact_source
    assert '"MeshCore QR  %.16s  type %u"' in contact_source
    assert "lv_qrcode_create" in contact_source
    assert "lv_qrcode_update" in contact_source

    detail = contact_source.split(
        "bool d1l_ui_contact_sheets_render_detail", 1
    )[1].split("bool d1l_ui_contact_sheets_render_options", 1)[0]
    for label in (
        '"Back"',
        '"Message"',
        '"Contact options"',
    ):
        assert label in detail
    assert '"Messaging unavailable"' in detail
    assert "d1l_ui_dm_identity_reason_text(" in detail
    for hidden_action in ('"Route trace"', '"Rename"', '"Export QR"', '"Forget contact"'):
        assert hidden_action not in detail

    options = contact_source.split(
        "bool d1l_ui_contact_sheets_render_options", 1
    )[1].split("bool d1l_ui_contact_sheets_render_forget", 1)[0]
    for option in (
        '"Route trace"',
        '"Rename"',
        '"Remove from favorites"',
        '"Add to favorites"',
        '"Include in unread count"',
        '"Exclude from unread count"',
        '"Export QR"',
        '"Forget contact"',
    ):
        assert option in options

    assert source.count("d1l_app_model_delete_contact(") == 1
    confirm = source.split(
        "case D1L_UI_CONTACT_ACTION_CONFIRM_FORGET:", 1
    )[1].split("case D1L_UI_CONTACT_ACTION_CLOSE_EXPORT:", 1)[0]
    assert "d1l_app_model_delete_contact(contact->fingerprint" in confirm

    probe = source.split("static lv_obj_t *scroll_probe_open_contact_surface", 1)[1].split(
        "static void measure_scroll_probe_target", 1
    )[0]
    for surface in ("contact_detail", "contact_options", "contact_forget", "contact_route"):
        assert f'"{surface}"' in probe
    assert "d1l_ui_contact_sheets_detail" in probe
    assert "d1l_ui_contact_sheets_options" in probe
    assert "d1l_ui_contact_sheets_forget" in probe
    assert "d1l_app_model_delete_contact(" not in probe

    run_probe = source.split("static void run_scroll_probe_on_ui_task", 1)[1].split(
        "static uint32_t begin_pending_scroll_probe", 1
    )[0]
    assert 'strncmp(canonical, "contact_", strlen("contact_")) == 0' in run_probe
    assert "result->ok = result->surface_supported && result->target_found;" in run_probe


def test_route_detail_sheet_opens_from_route_rows():
    source = read("main/ui/ui_phase1.c")
    assert "static lv_obj_t *s_route_detail_sheet" in source
    assert "static d1l_route_entry_t s_route_detail_route" in source
    assert "render_route_row" in source
    assert "render_route_detail_sheet" in source
    assert "open_route_detail_event_cb" in source
    assert "create_route_detail_sheet" in source
    assert "lv_obj_add_event_cb(row, open_route_detail_event_cb, LV_EVENT_CLICKED" in source
    assert "recent_route_count" in source
    assert "recent_routes" in source
    assert '"Routes"' in source
    assert "hide_route_detail_sheet()" in source


def test_packet_detail_sheet_opens_from_packet_rows():
    source = read("main/ui/ui_phase1.c")
    packets = read("main/ui/ui_packets.c")
    packets_header = read("main/ui/ui_packets.h")
    assert "static lv_obj_t *s_packet_detail_sheet" in source
    assert "static lv_obj_t *s_packet_search_sheet" in source
    assert "static d1l_packet_log_entry_t s_packet_detail_packet" in source
    assert "static d1l_ui_packets_controller_t s_packets_controller EXT_RAM_BSS_ATTR" in source
    assert "static bool s_packet_detail_advanced" in source
    assert "d1l_packet_log_entry_t rows[D1L_PACKET_LOG_CAPACITY]" in packets_header
    assert "#define D1L_UI_PACKETS_INITIAL_ROWS 100U" in packets_header
    assert "#define D1L_UI_PACKETS_LOAD_NEWER_STEP 100U" in packets_header
    assert "render_packet_detail_sheet" in source
    assert "open_packet_detail_event_cb" in source
    assert "open_packet_search_event_cb" in source
    assert "packet_filter_event_cb" in source
    assert "packet_pause_event_cb" in source
    assert "packet_load_older_event_cb" in source
    assert "packet_load_newer_event_cb" in source
    assert "packet_detail_mode_event_cb" in source
    assert "create_packet_detail_sheet" in source
    assert "create_packet_search_sheet" in source
    assert "lv_obj_add_event_cb(row, open_packet_detail_event_cb, LV_EVENT_CLICKED" in source
    assert "d1l_packet_log_query_page" not in packets
    assert "d1l_packet_log_query_page" in source
    assert "refresh_packet_terminal_rows" in source
    assert "d1l_ui_packets_query_request" in source
    assert "d1l_ui_packets_accept_query" in source
    assert "d1l_ui_packets_entry_color" in source
    assert "d1l_ui_packets_entry_status" in source
    assert '"All"' in source
    assert '"RX"' in source
    assert '"TX"' in source
    assert '"Text"' in source
    assert '"Search"' in source
    assert '"Pause"' in source
    assert '"Resume"' in source
    assert '"Load Older"' in source
    assert '"Newer"' in source
    assert '"Packet Feed"' in source
    assert '"page %u-%u/%u%s"' in source
    assert '"live %s  rssi %d  snr %s  avg %d"' in source
    assert '"Packet Search"' in source
    assert '"Search kind, note, raw hex"' in source
    assert '"Advanced"' in source
    assert '"Normal"' in source
    assert '"Raw Hex"' in source
    assert "raw_hex" in source
    assert "recent_packet_count" in source
    assert "s_packets_controller.rows[i]" in source
    assert "d1l_ui_packets_can_load_older(&s_packets_controller)" in source
    assert "d1l_ui_packets_can_load_newer(&s_packets_controller)" in source
    assert "hide_packet_detail_sheet()" in source
    assert "hide_packet_search_sheet()" in source


def test_settings_screen_renderer_has_an_owned_action_boundary():
    source = read("main/ui/ui_phase1.c")
    cmake = read("main/CMakeLists.txt")
    settings_module = read("main/ui/ui_settings.c")
    settings_header = read("main/ui/ui_settings.h")
    more_module = read("main/ui/ui_more_view.c")
    more_header = read("main/ui/ui_more_view.h")

    assert '"ui/ui_settings.c"' in cmake
    assert '"ui/ui_more_view.c"' in cmake
    assert '#include "ui_settings.h"' in source
    assert "d1l_ui_settings_render" in settings_module
    assert "d1l_ui_settings_render" in settings_header
    assert "lv_event_get_user_data(event)" in settings_module
    assert "(void *)(uintptr_t)action" not in settings_module
    assert "d1l_app_model_" not in settings_module
    assert "d1l_app_snapshot_t" not in settings_module
    assert "snapshot->" not in settings_module
    assert "static d1l_ui_settings_action_handler_t s_action_handler" not in settings_module
    assert "s_expanded_category" not in settings_module
    assert "k_action_values" not in settings_module
    assert "lv_obj_clean" not in settings_module
    assert "s_content" not in settings_module
    assert "static lv_obj_t *render_settings_tile" not in source

    wrapper = source.split(
        "static void render_settings(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)", 1
    )[1].split("\n}", 1)[0]
    assert "more_view_input_from_snapshot(snapshot, &input);" in wrapper
    assert "d1l_ui_more_view(&input, &s_settings_controller.rendered)" in wrapper
    assert "d1l_ui_more_view_model_t view_model" not in wrapper
    assert "&s_settings_controller.rendered" in wrapper
    assert "lv_" not in wrapper

    action_routes = (
        ("D1L_UI_SETTINGS_ACTION_PACKETS", "request_tab_switch(D1L_UI_TAB_PACKETS);"),
        ("D1L_UI_SETTINGS_ACTION_STORAGE", "open_storage_sheet_event_cb(NULL);"),
        ("D1L_UI_SETTINGS_ACTION_WIFI", "open_wifi_sheet_event_cb(NULL);"),
        ("D1L_UI_SETTINGS_ACTION_BLE", "open_ble_sheet_event_cb(NULL);"),
        ("D1L_UI_SETTINGS_ACTION_RADIO", "open_radio_settings_event_cb(NULL);"),
        ("D1L_UI_SETTINGS_ACTION_MAP_TILES", "open_map_options_sheet_event_cb(NULL);"),
        ("D1L_UI_SETTINGS_ACTION_DISPLAY", "open_display_sheet_event_cb(NULL);"),
        ("D1L_UI_SETTINGS_ACTION_DIAGNOSTICS", "open_diagnostics_sheet_event_cb(NULL);"),
        ("D1L_UI_SETTINGS_ACTION_ADVANCED", "open_sheet_event_cb(NULL);"),
    )
    for action, route in action_routes:
        assert action in more_header
        assert action in more_module
        route_body = source.split(f"case {action}:", 1)[1].split("break;", 1)[0]
        assert route in route_body

    for style in (
        "lv_obj_set_style_radius(row, 10, 0)",
        "lv_obj_set_style_bg_color(row, lv_color_hex(warning ? 0x231317 : 0x20262B), 0)",
        "lv_obj_set_style_border_color(row, lv_color_hex(warning ? 0x7F1D1D : 0x33404A), 0)",
        "lv_obj_set_style_border_width(row, 1, 0)",
        "lv_obj_set_style_pad_all(row, 0, 0)",
    ):
        assert style in settings_module
    assert "settings_create_row(parent, 444, 54, item->warning)" in settings_module
    assert "settings_create_container(group, 444)" in settings_module
    assert 'settings_create_label(parent, "Settings", 0xF4F7FB)' in settings_module
    assert '"Device, radio, network and support"' in settings_module
    assert "settings_category_event_cb" not in settings_module
    assert "settings_apply_category_state" not in settings_module
    assert "binding->generation != controller->generation" in settings_module
    assert "LV_OBJ_FLAG_HIDDEN" not in settings_module
    for category in ("Tools", "Connections", "Storage & maps", "Device", "Support", "Advanced"):
        assert f'"{category}"' in more_module


def test_settings_screen_reports_companion_wireless_state():
    source = read("main/ui/ui_phase1.c")
    wifi_module = read("main/ui/ui_wifi.c")
    wifi_header = read("main/ui/ui_wifi.h")
    ble_module = read("main/ui/ui_ble.c")
    ble_header = read("main/ui/ui_ble.h")
    device_sheets = read("main/ui/ui_device_sheets.c")
    device_sheets_header = read("main/ui/ui_device_sheets.h")
    settings_module = read("main/ui/ui_settings.c")
    more_module = read("main/ui/ui_more_view.c")
    connectivity = read("main/ui/ui_connectivity.c")
    header = read("main/app/app_model.h")
    assert "wifi_state" in header
    assert "ble_state" in header
    assert "wifi_profile_saved" in header
    assert "wifi_password_saved" in header
    assert "wifi_scan_supported" in header
    assert "wifi_stack_active" in header
    assert "wifi_connected" in header
    assert "wifi_connecting" in header
    assert "wifi_ssid" in header
    assert "wifi_ip" in header
    assert "wifi_last_error" in header
    assert "ble_transport_supported" in header
    assert "coexistence_policy" in header
    assert '"Tools"' in more_module
    assert '"Settings and utilities"' in more_module
    assert '"SD Card"' in more_module
    assert '"Map options"' in more_module
    assert '"Offline Maps"' not in more_module
    assert '"Identity"' in more_module
    assert '"Advanced"' in more_module
    assert '"About"' in more_module
    assert '"Packets"' in more_module
    assert '"Bluetooth"' in more_module
    assert "snapshot->identity_fingerprint" not in settings_module
    assert "format_radio_profile_line" not in settings_module
    assert "static d1l_ui_wifi_controller_t s_wifi_controller EXT_RAM_BSS_ATTR" in source
    assert "static d1l_ui_ble_controller_t s_ble_controller EXT_RAM_BSS_ATTR" in source
    assert "static d1l_ui_device_sheets_controller_t s_device_sheets_controller EXT_RAM_BSS_ATTR" in source
    assert "d1l_ui_wifi_create(&s_wifi_controller, s_screen)" in source
    assert "d1l_ui_ble_create(&s_ble_controller, s_screen)" in source
    assert "d1l_ui_device_sheets_create(&s_device_sheets_controller, s_screen)" in source
    assert "render_wifi_sheet" in source
    assert "d1l_ui_wifi_render(&s_wifi_controller" in source
    assert "render_ble_sheet" in source
    assert "render_display_sheet" in source
    assert "render_diagnostics_sheet" in source
    assert "open_wifi_sheet_event_cb" in source
    assert "open_ble_sheet_event_cb" in source
    assert "open_display_sheet_event_cb" in source
    assert "open_diagnostics_sheet_event_cb" in source
    assert '"Wi-Fi"' in wifi_module
    assert '"Bluetooth"' in ble_module
    assert '"Display"' in device_sheets
    assert '"Diagnostics"' in device_sheets
    assert "lv_obj_t *ssid_textarea;" in wifi_header
    assert "lv_obj_t *password_textarea;" in wifi_header
    assert "lv_obj_t *keyboard;" in wifi_header
    assert "d1l_ui_ble_controller_t" in ble_header
    assert "d1l_ui_device_sheets_controller_t" in device_sheets_header
    assert "d1l_app_model_save_wifi_profile(" in source
    assert "password && password[0] != '\\0' ? password : NULL" in source
    assert "d1l_app_model_wifi_scan(&s_wifi_scan_result)" in source
    assert "d1l_app_model_wifi_connect()" in source
    assert "d1l_app_model_delete_wifi_profile(" in source
    assert "d1l_app_model_select_wifi_profile(" in source
    assert "d1l_app_model_set_wifi_enabled(!s_snapshot.wifi_enabled)" in source
    assert "D1L_BLE_COMPANION_TRANSPORT_SUPPORTED true" in header
    app_model = read("main/app/app_model.c")
    assert "D1L_RELEASE_FEATURE_BLE" in app_model
    assert "D1L_BLE_COMPANION_TRANSPORT_SUPPORTED" in app_model
    assert "if (!controller->rendered.controls_available)" in wifi_module
    assert "lv_textarea_set_max_length(" in wifi_module
    assert "D1L_WIFI_SSID_LEN - 1U" in wifi_module
    assert "D1L_WIFI_PASSWORD_LEN - 1U" in wifi_module
    assert '"Network name (SSID)"' in wifi_module
    assert '"Password"' in wifi_module
    assert '"Scan to list nearby 2.4 GHz networks"' in connectivity
    assert '"Connect"' in wifi_module
    assert "D1L_UI_WIFI_ACTION_SCAN" in source
    assert "D1L_UI_WIFI_ACTION_CONNECT" in source
    assert '"Bluetooth companion connections are unavailable."' in connectivity
    assert '"Bluetooth companion connections are available."' in connectivity
    assert '"Bluetooth pairing is unavailable."' in connectivity
    assert '"Enable unavailable"' in ble_module
    assert '"Pair unavailable"' in ble_module
    assert '"Forget unavailable"' in ble_module
    assert "D1L_UI_BLE_ACTION_TOGGLE" in source
    assert '"Use Time -1h/+1h to set the local clock.' in device_sheets
    assert '"Terminal shows recent events and the current log level."' in device_sheets
    assert '"reset %s  uptime %lus  mesh %s"' in device_sheets


def test_settings_screen_has_safe_touch_radio_editor():
    source = read("main/ui/ui_phase1.c")
    radio_module = read("main/ui/ui_radio_settings.c")
    radio_header = read("main/ui/ui_radio_settings.h")
    more_module = read("main/ui/ui_more_view.c")
    model = read("main/app/app_model.c")

    assert "static lv_obj_t *s_radio_settings_sheet" not in source
    assert "static d1l_app_radio_profile_edit_t s_radio_edit" not in source
    assert "create_radio_settings_sheet" not in source
    assert "s_radio_settings_controller EXT_RAM_BSS_ATTR" in source
    assert "d1l_ui_radio_settings_create(" in source
    assert "render_radio_settings_sheet" in source
    assert "open_radio_settings_event_cb" in source
    assert "radio_settings_action_handler" in source
    assert "radio_edit_from_snapshot" in source
    assert "format_radio_profile_line" not in source
    assert '"Radio", radio_status' in more_module
    assert '"Radio"' in radio_module
    assert '"Canada preset"' in radio_module
    assert '"Radio matches saved settings"' in radio_module
    assert '"Saved settings apply when the radio restarts"' in radio_module
    assert '"Radio saved; RF apply pending"' in source
    assert '"Restore Canada"' in radio_module
    assert '"Save"' in radio_module
    assert '"Receive boost: On"' in radio_module
    assert "lv_obj_set_size(controller->sheet, 480, 480)" in radio_module
    assert "D1L_UI_RADIO_SETTINGS_ACTION_FREQ_DOWN" in radio_header
    assert "controller->edit.frequency_hz -= 25000UL" in radio_module
    assert "controller->edit.frequency_hz += 25000UL" in radio_module
    assert "next_bandwidth" in radio_module
    assert "controller->edit.rx_boost = !controller->edit.rx_boost" in radio_module
    assert "d1l_app_model_save_radio_profile(edit)" in source
    assert "d1l_app_model_default_radio_profile(&defaults)" in source
    assert "D1L_SETTINGS_UPDATE_RADIO_PROFILE" in model
    assert "settings.tcxo_mode = D1L_TCXO_NONE" in model


def test_settings_screen_has_non_destructive_storage_setup_sheet():
    source = read("main/ui/ui_phase1.c")
    storage_view = read("main/ui/ui_storage_view.c")
    header = read("main/app/app_model.h")
    model = read("main/app/app_model.c")

    assert "static lv_obj_t *s_storage_sheet" in source
    assert "create_storage_sheet" in source
    assert "render_storage_sheet" in source
    assert "open_storage_sheet_event_cb" in source
    assert "hide_storage_sheet" in source
    assert 'render_storage_header("Storage"' in source
    assert 'render_storage_header("Card status"' in source
    assert 'render_storage_header("Data locations"' in source
    assert "D1L_STORAGE_PAGE_CARD_STATUS" in source
    assert "D1L_STORAGE_PAGE_DATA_LOCATIONS" in source
    assert '"next step: %s%s"' not in source
    assert "static const char *card_state" in storage_view
    assert "FAT32 only - This device never formats cards." in source
    assert "static const char *retained_backend" in storage_view
    assert "static const char *map_backend" in storage_view
    assert "static const char *export_backend" in storage_view
    assert "storage_setup_action" in header
    assert "storage_sd_needs_fat32" in header
    assert "storage_rp2040_sd_protocol_supported" in header
    assert "snapshot->storage_setup_action = storage.setup_action" in model
    assert "snapshot->storage_sd_needs_fat32 = storage.sd_needs_fat32" in model


def test_live_packet_receive_store_reads_are_serialized_for_ui_snapshots():
    lock_header = read("main/mesh/store_lock.h")
    ui_source = read("main/ui/ui_phase1.c")
    assert "xSemaphoreCreateMutexStatic" in lock_header
    assert "d1l_store_lock_take" in lock_header
    assert "d1l_store_lock_give" in lock_header
    assert ".buffer = {0}" not in lock_header

    for path in [
        "main/mesh/packet_log.c",
        "main/mesh/message_store.c",
        "main/mesh/dm_store.c",
        "main/mesh/node_store.c",
        "main/mesh/route_store.c",
        "main/mesh/contact_store.c",
        "main/mesh/mesh_inspector.c",
    ]:
        source = read(path)
        assert '#include "mesh/store_lock.h"' in source, path
        assert "D1L_STORE_LOCK_INITIALIZER" in source, path
        assert "d1l_store_lock_take" in source, path
        assert "d1l_store_lock_give" in source, path

    refresh_body = ui_source.split("static void refresh_timer_cb", 1)[1].split(
        "static void create_top_bar", 1
    )[0]
    assert "render_active_tab()" not in refresh_body
    assert "content_generation_changed_from_rendered(&s_snapshot)" in refresh_body
    assert "request_content_refresh();" in refresh_body
    assert "message_total_written" in ui_source
    assert "dm_total_written" in ui_source
    assert "node_total_written" in ui_source
    assert "route_total_written" in ui_source
    assert "packet_total_written" in ui_source


def test_compose_probe_is_tx_suppressed_without_weakening_dm_admission():
    source = read("main/ui/ui_phase1.c")

    production = source.split(
        "static void open_dm_compose_for_contact", 1
    )[1].split("static void open_node_dm_for", 1)[0]
    assert "dm_identity_for_contact(entry, &selected)" in production
    assert production.index("if (!eligibility.can_open_compose)") < (
        production.index("present_dm_compose_sheet(&selected, false)")
    )

    probe = source.split(
        "static void open_compose_probe_on_ui_task", 1
    )[1].split("static void open_keyboard_probe_on_ui_task", 1)[0]
    assert "present_dm_compose_sheet(&contact, true)" in probe
    assert "open_dm_compose_for_contact(&contact)" not in probe
    assert "s_compose_probe_send_suppressed = true" in probe

    send = source.split("static void send_compose_text(void)", 1)[1].split(
        "static void send_compose_event_cb", 1
    )[0]
    suppression = send.index("if (s_compose_probe_send_suppressed)")
    assert suppression < send.index("d1l_app_model_send_dm_text")
    assert suppression < send.index("d1l_app_model_send_channel_text")
    assert "return;" in send[suppression:send.index("const char *text")]

    hide = source.split("static void hide_compose_sheet(void)", 1)[1].split(
        "static void hide_public_history_sheet", 1
    )[0]
    assert "s_compose_probe_send_suppressed = false" in hide
