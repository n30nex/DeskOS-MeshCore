from pathlib import Path

from scripts.smoke_d1l import SMOKE_COMMANDS


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_public_message_store_is_bounded_and_retained_blob_store_backed():
    header = read("main/mesh/message_store.h")
    source = read("main/mesh/message_store.c")
    blob_store = read("main/storage/retained_blob_store.c")
    cmake = read("main/CMakeLists.txt")
    assert "D1L_MESSAGE_STORE_CAPACITY 16U" in header
    assert "D1L_MESSAGE_MAX_BYTES D1L_USER_TEXT_MAX_BYTES" in header
    assert "D1L_MESSAGE_MAX_CHARS D1L_MESSAGE_MAX_BYTES" in header
    assert "D1L_MESSAGE_TEXT_LEN (D1L_MESSAGE_MAX_BYTES + 1U)" in header
    assert "D1L_MESSAGE_STORE_SCHEMA 5U" in source
    assert "D1L_MESSAGE_STORE_SCHEMA_V4 4U" in source
    assert "D1L_MESSAGE_STORE_SCHEMA_V3 3U" in source
    assert "D1L_MESSAGE_STORE_SCHEMA_V2 2U" in source
    assert "D1L_MESSAGE_STORE_SCHEMA_V1 1U" in source
    assert "convert_v2_blob" in source
    assert "convert_v1_blob" in source
    assert "convert_v3_blob" in source
    assert "convert_v4_blob" in source
    assert 'D1L_RETAINED_PUBLIC_MESSAGE_NAMESPACE "d1l_messages"' in blob_store
    assert 'D1L_RETAINED_PUBLIC_MESSAGE_SD_DIR "stores/messages/public"' in blob_store
    assert "D1L_MESSAGE_STORE_ID D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES" in source
    assert '#include "storage/retained_blob_store.h"' in source
    assert "d1l_retained_blob_store_backend_state(D1L_MESSAGE_STORE_ID" in source
    assert "d1l_retained_blob_store_read_sd_primary(" in source
    assert "d1l_retained_blob_store_read_nvs_fallback(" in source
    assert "d1l_retained_blob_store_write_sd_primary_guarded(" in source
    assert "d1l_retained_blob_store_write_nvs_fallback(" in source
    assert "d1l_retained_blob_store_erase_sd_primary_guarded(" not in source
    assert source.count("d1l_retained_blob_store_erase_nvs_fallback(") == 2
    clear_all = source.split("esp_err_t d1l_message_store_clear(void)", 1)[1].split(
        "esp_err_t d1l_message_store_clear_channel", 1
    )[0]
    clear_channel = source.split(
        "esp_err_t d1l_message_store_clear_channel", 1
    )[1].split("esp_err_t d1l_message_store_flush", 1)[0]
    for clear in (clear_all, clear_channel):
        retire = clear.index("d1l_retained_blob_store_erase_nvs_fallback(")
        flush = clear.index("d1l_message_store_flush()")
        assert retire < flush
        assert "if (retire_ret != ESP_OK)" in clear
    assert "s_epoch" in source
    assert "s_content_revision" in source
    assert "s_clear_lineage" in source
    assert "new_clear_lineage" in source
    assert "esp_random()" in source
    assert "s_device_lineage_authoritative" in source
    assert "clear_lineage" in header
    assert "bool loaded" in header
    assert ".loaded = s_loaded" in source
    assert "sd_backend_generation_matches" in source
    assert "reconcile_sd_primary" in source
    assert "s_sd_reconcile_pending" in source
    assert "s_sd_primary_dirty" in source
    assert "s_nvs_fallback_dirty" in source
    assert "s_persistence_stale_snapshot_count" in source
    assert "esp_err_t d1l_message_store_flush(void)" in source
    assert "esp_err_t d1l_message_store_flush_if_due(void)" in source
    assert "static d1l_store_lock_t s_init_recovery_lock" in source
    assert "static esp_err_t ensure_store_initialized(void)" in source
    assert "d1l_store_lock_take(&s_init_recovery_lock)" in source
    assert "d1l_retained_blob_store_write(D1L_MESSAGE_STORE_ID" not in source
    assert "nvs_get_blob" not in source
    assert "nvs_set_blob" not in source
    assert "static d1l_message_store_blob_t s_blob_scratch" in source
    assert "d1l_message_entry_t incoming[D1L_MESSAGE_STORE_CAPACITY]" not in source
    assert "d1l_message_store_copy_recent" in source
    assert "d1l_message_store_copy_channel_recent" in source
    assert "d1l_message_store_query_page" in header
    assert "d1l_message_store_query_channel_page" in header
    assert "d1l_message_store_query" in header
    assert "skip_newest" in source
    assert "out_total_matches" in source
    assert "contains_casefold" in source
    assert "message_matches_query" in source
    assert '"mesh/message_store.c"' in cmake
    assert '"mesh/user_text.c"' in cmake
    assert "d1l_user_text_validate_bounded(entry->text" in source
    assert "legacy_entry_is_valid" in source
    assert "memcpy(entry.text, text, text_info.byte_count + 1U)" in source
    assert "entry->channel_id != 0U" in source
    assert "offsetof(d1l_message_entry_t, channel_id) == 192U" in source
    assert "sizeof(d1l_message_entry_t) == 200U" in source
    assert "d1l_message_store_clear_channel" in header


def test_public_rf_appends_to_message_store():
    source = read("main/mesh/meshcore_service.c")
    assert "d1l_message_store_append_channel_deferred(" in source
    assert "remember_pending_channel_tx(" in source
    assert "maintain_pending_channel_history(" in source
    assert "append_channel_message_store_rx(" in source
    assert "reconcile_channel_messages(message_seq)" in source


def test_ui_and_console_expose_persistent_public_messages():
    app_header = read("main/app/app_model.h")
    app_source = read("main/app/app_model.c")
    ui = read("main/ui/ui_phase1.c")
    messages_ui = read("main/ui/ui_messages.c")
    console = read("main/comms/usb_console.c")
    detail = ui.split("static void render_message_detail_sheet(void)", 1)[1].split(
        "static void show_message_detail_for", 1
    )[0]
    assert "D1L_APP_SNAPSHOT_MESSAGE_PREVIEW 5U" in app_header
    assert "recent_messages" in app_header
    assert "d1l_app_model_query_public_messages_page" in app_header
    assert "d1l_app_model_query_public_messages" in app_header
    assert "d1l_message_store_copy_recent" in app_source
    assert "d1l_message_store_query_page(out_entries, max_entries, skip_newest" in app_source
    assert "messages_render_public_row" in messages_ui
    assert "render_message_detail_sheet" in ui
    assert "show_message_detail_for" in ui
    assert "s_message_detail_message = *entry" in ui
    assert 'create_nested_page_body(s_message_detail_sheet, "message detail body")' in detail
    assert 'create_nested_page_label(body, entry->text[0] ? entry->text : "-", 0xF4F7FB, true)' in detail
    assert detail.index('entry->text[0] ? entry->text : "-"') < detail.index(
        '"Technical details"'
    )
    assert "lv_textarea_set_max_length(s_compose_textarea, D1L_MESSAGE_MAX_CHARS)" in ui
    assert "static lv_obj_t *s_compose_counter" in ui
    assert "update_compose_counter()" in ui
    assert "LV_EVENT_VALUE_CHANGED" in ui
    assert "No messages in this channel yet" in messages_ui
    assert "No direct-message history yet." in messages_ui
    assert "Loading retained direct-message history..." in messages_ui
    assert "Storage degraded; readable RAM history remains." in messages_ui
    assert "Persistence unavailable; readable RAM history remains." in messages_ui
    assert "static lv_obj_t *s_public_history_sheet" in ui
    assert "static lv_obj_t *s_public_search_sheet" in ui
    assert "d1l_app_model_query_channel_messages_page(" in ui
    assert "s_public_history_channel_id, s_public_history_entries" in ui
    assert "public_history_load_older_event_cb" in ui
    assert "D1L_UI_MESSAGES_ACTION_OPEN_HISTORY" in messages_ui
    assert "create_public_history_sheet" in ui
    assert "create_public_search_sheet" in ui
    assert 'lv_textarea_set_placeholder_text(s_public_search_textarea, "Search author or message")' in ui
    assert 'ok_begin("messages public")' in console
    assert 'strcmp(line, "messages public")' in console
    assert 'strncmp(args, "search ", 7)' in console
    assert "messages public [offset <n>]" in console
    assert "messages public search <text> [offset <n>]" in console
    assert '\\"page_size\\"' in console
    assert '\\"total_matches\\"' in console
    assert '\\"has_older\\"' in console
    assert '\\"next_offset\\"' in console
    assert "optional search filters retained rows and offset pages older rows" in console
    assert "Public messages are kept in bounded retained storage" in console
    assert "Public messages are kept in a bounded NVS store" not in console
    assert "d1l_message_store_query_page_snapshot(" in console
    assert "D1L_CONSOLE_MESSAGE_PAGE_SIZE" in console
    assert "messages public" in SMOKE_COMMANDS
    assert "messages public search test" in SMOKE_COMMANDS
