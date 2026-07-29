from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_dm_thread_controller_owns_bounded_search_state() -> None:
    header = read("main/ui/ui_messages.h")
    source = read("main/ui/ui_messages.c")

    assert "D1L_UI_MESSAGES_ACTION_OPEN_DM_SEARCH" in header
    assert "char thread_search_text[D1L_MESSAGE_TEXT_LEN]" in header
    assert "D1L_UI_MESSAGES_THREAD_MAX_ROWS (D1L_DM_STORE_CAPACITY + 1U)" in header
    assert "const char *query" in header
    assert "d1l_ui_messages_set_thread_search" in header
    assert "d1l_ui_messages_thread_search" in header

    render = source.split("bool d1l_ui_messages_render_thread(", 1)[1].split(
        "bool d1l_ui_messages_expand_thread", 1
    )[0]
    assert "controller->thread_search_text" in render
    assert "D1L_UI_MESSAGES_THREAD_MAX_ROWS" in render
    assert "d1l_ui_messages_thread_row_index_valid(controller, index)" in source
    assert render.index("controller->thread_search_text") < render.index(
        "&controller->thread_total_matches"
    )
    assert 'sheet, "Search", 376, 6, 88, 44' in render
    assert "D1L_UI_MESSAGES_ACTION_OPEN_DM_SEARCH" in render
    assert '"Showing %u of %u | search active"' in render
    assert "controller->thread_fingerprint" not in render.split(
        "char meta_text[96];", 1
    )[1].split("lv_obj_t *meta", 1)[0]
    assert "No retained messages match this search." in render
    assert "No retained messages in this conversation." in render

    setter = source.split("bool d1l_ui_messages_set_thread_search", 1)[1].split(
        "const char *d1l_ui_messages_thread_search", 1
    )[0]
    assert "while (length < sizeof(controller->thread_search_text)" in setter
    assert "memcpy(controller->thread_search_text, source, length + 1U)" in setter
    assert "D1L_UI_MESSAGES_THREAD_INITIAL_ROWS" in setter
    assert "expanded_row_valid = false" in setter
    assert "memset(controller->thread_entries" in setter
    assert "memset(controller->thread_unread" in setter

    select = source.split("bool d1l_ui_messages_select_thread", 1)[1].split(
        "bool d1l_ui_messages_render_thread", 1
    )[0]
    hide = source.split("void d1l_ui_messages_hide_thread", 1)[1].split(
        "void d1l_ui_messages_deactivate", 1
    )[0]
    assert "thread_search_text[0] = '\\0'" in select
    assert "messages_text_fits(" in select
    assert "thread_search_text[0] = '\\0'" in hide


def test_dm_search_ui_is_thread_scoped_read_only_and_refresh_stable() -> None:
    source = read("main/ui/ui_phase1.c")

    loader = source.split("static size_t load_dm_thread_rows", 1)[1].split(
        "static bool render_dm_thread_sheet", 1
    )[0]
    assert "const char *query" in loader
    assert "d1l_app_model_query_dm_thread_page(" in loader
    assert "query, out_total_matches" in loader

    apply_search = source.split("static void apply_dm_search_event_cb", 1)[1].split(
        "static void clear_dm_search_event_cb", 1
    )[0]
    clear_search = source.split("static void clear_dm_search_event_cb", 1)[1].split(
        "static void dm_search_keyboard_event_cb", 1
    )[0]
    open_search = source.split("static void open_dm_search_event_cb", 1)[1].split(
        "static void show_dm_thread_for", 1
    )[0]
    for block in (apply_search, clear_search, open_search):
        assert "d1l_app_model_mark_dm_thread_read" not in block
        assert "d1l_app_model_send_dm_text" not in block
        assert "d1l_app_model_send_public_text" not in block
    assert "d1l_ui_messages_set_thread_search" in apply_search
    assert "d1l_ui_messages_set_thread_search(&s_messages_controller, NULL)" in clear_search
    assert "d1l_ui_messages_thread_search" in open_search

    refresh = source.split("static void process_pending_content_refresh", 1)[1].split(
        "static lv_obj_t *find_scrollable_descendant", 1
    )[0]
    assert "dm_search_visible" in refresh
    assert "dm_search_visible ? s_dm_search_sheet" in refresh

    create = source.split("static void create_dm_search_sheet", 1)[1].split(
        "static void create_storage_sheet", 1
    )[0]
    assert '"DM Search"' in create
    assert '"Search this conversation"' in create
    assert '"Apply", 212, 0, 66, 44' in create
    assert '"Clear", 286, 0, 64, 44' in create
    assert '"Close", 358, 0, 66, 44' in create
    assert "d1l_ui_keyboard_configure_input(" in create


def test_dm_search_is_in_simulator_and_future_hardware_gate() -> None:
    simulator = read("tools/ui_simulator.py")
    capture = read("scripts/ui_compose_keyboard_capture_d1l.py")
    audit = read("scripts/release_gate_audit_d1l.py")
    console = read("main/comms/usb_console.c")

    for view in (
        "dm_search_sheet",
        "dm_thread_search_results",
        "dm_thread_search_no_match",
        "dm_thread_empty_sheet",
    ):
        assert f'"{view}"' in simulator
    assert '"dm-search"' in capture
    assert '"dm-search"' in audit
    assert "public-search|dm-search|packet-search" in console
    phase1 = read("main/ui/ui_phase1.c")
    assert 'strcmp(canonical, "dm_search") == 0' in phase1
    assert "s_messages_mode == D1L_UI_MESSAGES_MODE_DIRECT" in phase1
