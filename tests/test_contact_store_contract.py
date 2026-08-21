import json
import os
import shutil
import subprocess
from pathlib import Path
from urllib.parse import unquote_plus

from scripts.smoke_d1l import SMOKE_COMMANDS


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def static_void_body(source: str, symbol: str) -> str:
    marker = f"static void {symbol}("
    start = source.rfind(marker)
    assert start >= 0, symbol
    end = source.find("\nstatic ", start + len(marker))
    return source[start : end if end >= 0 else len(source)]


def c_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def test_contact_store_is_bounded_and_sd_first():
    header = read("main/mesh/contact_store.h")
    source = read("main/mesh/contact_store.c")
    cmake = read("main/CMakeLists.txt")
    app_main = read("main/app_main.c")
    sdkconfig_defaults = read("sdkconfig.defaults")
    assert "D1L_CONTACT_STORE_CAPACITY 16U" in header
    assert "D1L_CONTACT_ALIAS_LEN 32U" in header
    assert "D1L_CONTACT_OUT_PATH_MAX 64U" in header
    assert "public_key_hex" in header
    assert "out_path_valid" in header
    assert "D1L_CONTACT_STORE_SCHEMA 7U" in source
    assert "D1L_CONTACT_STORE_SCHEMA_V6 6U" in source
    assert "D1L_CONTACT_STORE_SCHEMA_V5 5U" in source
    assert "D1L_CONTACT_STORE_SCHEMA_V4 4U" in source
    assert "D1L_CONTACT_STORE_SCHEMA_V3 3U" in source
    assert "D1L_CONTACT_STORE_LEGACY_TYPE_LEN 8U" in source
    assert "d1l_contact_store_blob_v3_t" in source
    assert "d1l_contact_store_blob_v4_t" in source
    assert "d1l_contact_store_blob_v5_t" in source
    assert "d1l_contact_store_blob_v6_t" in source
    assert "d1l_contact_store_blob_v2_t" in source
    assert "d1l_contact_store_blob_v1_t" in source
    assert "migrate_v1_blob" in source
    assert "migrate_v2_blob" in source
    assert "migrate_v3_blob" in source
    assert "migrate_v4_blob" in source
    assert "migrate_v5_blob" in source
    assert "migrate_v6_blob" in source
    assert "migrate_legacy_advert_type" in source
    assert "contact schema v1 layout changed" in source
    assert "contact schema v2 layout changed" in source
    assert "contact schema v3 layout changed" in source
    assert "contact schema v4 layout changed" in source
    assert "contact schema v5 layout changed" in source
    assert "contact schema v6 layout changed" in source
    assert "contact schema v7 layout changed" in source
    assert 'D1L_CONTACT_STORE_KEY "contacts"' in source
    assert '#include "storage/retained_blob_store.h"' in source
    assert "D1L_RETAINED_BLOB_STORE_CONTACTS" in source
    assert "d1l_retained_blob_store_read(" in source
    assert "d1l_retained_blob_store_read_sd_primary(" in source
    assert "d1l_retained_blob_store_write_sd_primary_guarded(" in source
    assert "d1l_retained_blob_store_erase_sd_primary_guarded(" in source
    assert "d1l_retained_blob_store_erase_nvs_fallback(" in source
    assert "nvs_get_blob" not in source
    assert "nvs_set_blob" not in source
    assert "static d1l_contact_store_blob_t s_blob_scratch" in source
    assert "static d1l_contact_store_blob_t s_rollback_scratch" in source
    assert "persist_store_or_rollback" in source
    assert "find_index_by_fingerprint" in source
    assert "oldest_evictable_placeholder_index" in source
    assert "d1l_contact_store_update_path" in source
    assert "d1l_contact_store_update_path_from_source" in source
    assert "d1l_contact_store_prepare_path_route" in source
    assert "d1l_contact_store_note_path_result" in source
    assert "retained_path_record_is_valid" in source
    assert "D1L_CONTACT_PATH_PERSIST_MIN_INTERVAL_MS 1000U" in header
    assert "d1l_contact_store_flush" in header
    assert "d1l_contact_store_flush_if_due" in header
    assert "persistence_revision" in header
    assert "saturates and rejects further mutations" in header
    assert "persistence_commit_count" in header
    assert "persistence_coalesced_count" in header
    assert "persistence_fail_count" in header
    assert "persistence_dirty" in header
    assert "d1l_contact_store_set_flags" in source
    assert "d1l_contact_store_rename" in header
    assert "d1l_contact_store_delete" in header
    assert "esp_err_t d1l_contact_store_rename" in source
    assert "esp_err_t d1l_contact_store_delete" in source
    assert source.count("persist_store_or_rollback(&s_rollback_scratch)") >= 5
    assert "d1l_contact_entry_t removed = s_entries[index]" in source
    assert "D1L_CONTACT_EXPORT_URI_LEN 224U" in header
    assert "d1l_contact_store_export_uri" in header
    assert "d1l_contact_uri_format" in source
    assert "D1L_CONTACT_URI_SCHEME" in read("main/mesh/contact_uri.c")
    assert "d1l_contact_store_meshcore_type_id" in source
    assert "d1l_contact_store_has_export_key" in source
    assert "CONFIG_LV_USE_QRCODE=y" in sdkconfig_defaults
    assert '"mesh/contact_store.c"' in cmake
    assert '"mesh/meshcore_path_state.c"' in cmake
    assert '"mesh/contact_uri.c"' in cmake
    assert "d1l_contact_store_init()" in app_main

    for signature in (
        "esp_err_t d1l_contact_store_update_path_from_source(",
        "esp_err_t d1l_contact_store_prepare_path_route(",
        "esp_err_t d1l_contact_store_note_path_result(",
    ):
        body = c_function(source, signature)
        assert "mark_deferred_persistence_locked(" in body
        assert "fill_blob(" not in body
        assert "persist_store" not in body
        assert "nvs_" not in body

    deferred_flush = c_function(source, "static esp_err_t flush_deferred_path_state(")
    assert "fill_blob(&s_persist_snapshot)" in deferred_flush
    assert "d1l_retained_blob_store_write_sd_primary_guarded(" in deferred_flush
    assert "d1l_retained_blob_store_erase_sd_primary_guarded(" in deferred_flush
    assert "contact_sd_backend_generation_matches(" in deferred_flush
    assert "s_persist_io_lock" in deferred_flush
    revision = c_function(source, "static esp_err_t reserve_persistence_revision_locked(")
    assert "revision == UINT32_MAX" in revision
    assert "revision + 1U" in revision
    assert "__atomic_add_fetch" not in revision
    sequence = c_function(source, "static esp_err_t reserve_sequenced_mutation_locked(")
    assert "s_next_seq == UINT32_MAX" in sequence
    assert "reserve_persistence_revision_locked(true)" in sequence
    assert source.count("entry->seq = s_next_seq++;") == 9
    for signature in (
        "esp_err_t d1l_contact_store_upsert_from_node(",
        "esp_err_t d1l_contact_store_upsert_verified_advert(",
        "esp_err_t d1l_contact_store_import_uri(",
        "esp_err_t d1l_contact_store_update_path_from_source(",
        "esp_err_t d1l_contact_store_reset_path(",
        "esp_err_t d1l_contact_store_prepare_path_route(",
        "esp_err_t d1l_contact_store_note_path_result(",
        "esp_err_t d1l_contact_store_set_flags(",
        "esp_err_t d1l_contact_store_rename(",
    ):
        mutation = c_function(source, signature)
        reserve_at = mutation.index("reserve_sequenced_mutation_locked()")
        seq_at = mutation.index("entry->seq = s_next_seq++;")
        assert reserve_at < seq_at
    assert "return force ? ESP_ERR_INVALID_STATE : ESP_OK" in deferred_flush
    assert "result = ESP_ERR_INVALID_STATE" in deferred_flush

    init = c_function(source, "esp_err_t d1l_contact_store_init(")
    clear = c_function(source, "esp_err_t d1l_contact_store_clear(")
    assert "reserve_persistence_revision_locked(false)" in clear
    assert "nvs_erase_key" not in init
    assert "ESP_ERR_NOT_SUPPORTED" in init
    assert "ESP_ERR_INVALID_STATE" in init
    assert "D1L_CONTACT_MUTATION_AUTHORITY_EXPLICIT_CLEAR" in clear
    assert "persist_store()" in clear


def test_contact_sd_generation_reconciliation_is_guarded_and_identity_scoped():
    header = read("main/mesh/contact_store.h")
    source = read("main/mesh/contact_store.c")

    for field in (
        "sd_backend_generation",
        "sd_primary_required",
        "sd_primary_reconcile_pending",
    ):
        assert field in header

    persist = c_function(source, "static esp_err_t persist_store(")
    assert persist.index("d1l_retained_blob_store_backend_state(") < persist.index(
        "d1l_retained_blob_store_write_sd_primary_guarded("
    )
    assert persist.index("reconcile_sd_primary_locked(") < persist.index(
        "d1l_retained_blob_store_write_sd_primary_guarded("
    )
    assert persist.count("contact_sd_backend_generation_matches(") >= 2
    assert "s_sd_reconcile_pending = true;" in persist

    reconcile = c_function(source, "static esp_err_t reconcile_sd_primary_locked(")
    assert reconcile.index("read_current_sd_blob(") < reconcile.index(
        "merge_contact_blobs_locked("
    )
    assert "D1L_CONTACT_MUTATION_AUTHORITY_NONE" in reconcile
    assert "D1L_CONTACT_MUTATION_AUTHORITY_EXPLICIT_CLEAR" in reconcile
    assert "contact_blobs_equivalent(" in reconcile

    merge = c_function(source, "static esp_err_t merge_contact_blobs_locked(")
    assert "fingerprint_was_deleted_locked(" in merge
    assert "fingerprint_was_touched_locked(" in merge
    assert merge.index("*candidate") < merge.index(
        "fingerprint_was_touched_locked("
    )

    release = c_function(
        source, "static void release_durable_mutation_authority_locked("
    )
    assert "D1L_CONTACT_MUTATION_AUTHORITY_NONE" in release
    assert "s_deleted_fingerprint_count = 0U;" in release
    assert "s_touched_fingerprint_count = 0U;" in release
    clear = c_function(source, "esp_err_t d1l_contact_store_clear(")
    assert "release_durable_mutation_authority_locked();" in clear


def test_contacts_can_promote_heard_nodes_by_fingerprint():
    node_header = read("main/mesh/node_store.h")
    node_source = read("main/mesh/node_store.c")
    console = read("main/comms/usb_console.c")
    assert "d1l_node_store_find_by_fingerprint" in node_header
    assert "bool d1l_node_store_find_by_fingerprint" in node_source
    assert "parse_fingerprint_token" in console
    assert "dest[i] = (char)tolower(c);" in console
    assert "dest[i] = (char)toupper(c);" not in console
    assert "d1l_node_store_find_by_fingerprint(fingerprint, &heard)" in console
    assert "d1l_contact_store_upsert_from_node(fingerprint, alias" in console
    upsert = read("main/mesh/contact_store.c").split(
        "esp_err_t d1l_contact_store_upsert_from_node", 1
    )[1].split("esp_err_t d1l_contact_store_upsert_verified_advert", 1)[0]
    assert "heard_node->public_key_hex" not in upsert
    assert "A passive observation is not identity authorization" in upsert
    assert 'ok_begin("contacts add")' in console
    assert '\\"public_key\\"' in console
    assert 'strcmp(line, "contacts")' in console
    assert 'strcmp(line, "contacts export")' in console
    assert 'strncmp(line, "contacts export ", 16)' in console
    assert 'strncmp(line, "contacts import ", 16)' in console
    assert 'strcmp(line, "contacts clear")' in console
    assert 'strncmp(line, "contacts add ", 13)' in console
    assert 'strncmp(line, "contacts rename ", 16)' in console
    assert 'strncmp(line, "contacts delete ", 16)' in console
    assert 'strncmp(line, "contacts set ", 13)' in console
    assert "d1l_contact_store_set_flags(fingerprint, contact.favorite, contact.muted, &contact)" in console
    assert "d1l_contact_store_rename(fingerprint, alias, &contact)" in console
    assert "d1l_app_model_delete_contact(fingerprint, &contact)" in console
    assert 'contacts set <fingerprint> <favorite|mute> <0|1>' in console
    assert 'contacts rename <fingerprint> <alias>' in console
    assert 'contacts delete <fingerprint>' in console


def test_ui_console_and_smoke_expose_contacts():
    app_header = read("main/app/app_model.h")
    app_source = read("main/app/app_model.c")
    ui = read("main/ui/ui_phase1.c")
    contact_ui = read("main/ui/ui_contact_sheets.c")
    contact_header = read("main/ui/ui_contact_sheets.h")
    nodes_ui = read("main/ui/ui_nodes.c")
    console = read("main/comms/usb_console.c")
    assert "recent_contacts" in app_header
    assert "contact_total_written" in app_header
    assert "d1l_contact_store_copy_recent" in app_source
    assert "d1l_app_model_set_contact_flags" in app_header
    assert "d1l_app_model_rename_contact" in app_header
    assert "d1l_app_model_delete_contact" in app_header
    assert "d1l_app_model_export_contact_uri" in app_header
    assert "d1l_contact_store_export_uri(&contact, dest, dest_size)" in app_source
    assert "d1l_contact_store_set_flags(fingerprint, favorite, muted, out_contact)" in app_source
    assert "d1l_contact_store_rename(fingerprint, alias, out_contact)" in app_source
    assert "d1l_contact_store_delete(fingerprint, out_contact)" in app_source
    assert "d1l_admin_credential_store_forget(fingerprint)" in app_source
    delete_contact = app_source.split(
        "esp_err_t d1l_app_model_delete_contact", 1
    )[1].split("esp_err_t d1l_app_model_export_contact_uri", 1)[0]
    assert delete_contact.count(
        "d1l_contact_store_delete(fingerprint, out_contact)"
    ) == 2
    assert "delete_ret == ESP_ERR_INVALID_STATE" in delete_contact
    assert "delete_ret == ESP_ERR_TIMEOUT" in delete_contact
    assert "delete_ret == ESP_FAIL" in delete_contact
    assert "nodes_render_contact_row" in nodes_ui
    contact_row = nodes_ui.split(
        "static void nodes_render_contact_row", 1
    )[1].split("static void nodes_render_node_row", 1)[0]
    assert "nodes_role_badge_text(entry->type)" in contact_row
    assert "nodes_contact_route_label(entry)" in contact_row
    assert 'nodes_create_button(row, "Chat"' in contact_row
    assert "entry->public_key_hex" not in contact_row
    assert "entry->fingerprint" not in contact_row
    assert "d1l_ui_contact_sheets_create(" in ui
    for renderer in ("detail", "options", "forget", "edit", "export"):
        assert f"d1l_ui_contact_sheets_render_{renderer}" in contact_ui
    assert "D1L_UI_CONTACT_ACTION_RENAME" in contact_header
    assert "d1l_app_model_rename_contact(" in ui
    assert "lv_qrcode_create" in contact_ui
    assert "lv_qrcode_update" in contact_ui
    assert "update_contact_detail_flags" in ui
    assert "D1L_UI_CONTACT_ACTION_TOGGLE_FAVORITE" in contact_header
    assert "D1L_UI_CONTACT_ACTION_TOGGLE_MUTE" in contact_header

    delete_call = "d1l_app_model_delete_contact(contact->fingerprint"
    confirm = ui.split("case D1L_UI_CONTACT_ACTION_CONFIRM_FORGET:", 1)[1].split(
        "case D1L_UI_CONTACT_ACTION_CLOSE_EXPORT:", 1
    )[0]
    assert ui.count(delete_call) == 1
    assert delete_call in confirm
    for action in (
        "CANCEL_FORGET",
        "CANCEL_EDIT",
        "SAVE_EDIT",
        "CLOSE_EXPORT",
    ):
        branch = ui.split(f"case D1L_UI_CONTACT_ACTION_{action}:", 1)[1].split(
            "case D1L_UI_CONTACT_ACTION_", 1
        )[0]
        assert delete_call not in branch
        assert "show_contact_options_sheet();" in branch

    assert '"Contacts"' in nodes_ui
    assert 'ok_begin("contacts")' in console
    assert '\\"out_path_known\\"' in console
    assert '\\"out_path_len\\"' in console
    assert '\\"meshcore_uri\\"' in console
    assert "Canonical contacts require a full-key signed advert or validated MeshCore URI import" in console
    assert "contacts" in SMOKE_COMMANDS
    assert "contacts export" in SMOKE_COMMANDS


def test_console_contact_snapshot_covers_full_bounded_store():
    console = read("main/comms/usb_console.c")
    body = console.split("static void cmd_contacts(void)", 1)[1].split(
        "static const char *contact_import_result_name", 1
    )[0]

    assert "entries[D1L_CONTACT_STORE_CAPACITY]" in body
    assert (
        "d1l_contact_store_copy_recent(\n"
        "        entries, D1L_CONTACT_STORE_CAPACITY)"
    ) in body
    assert "entries[8]" not in body


def test_core_retained_witness_never_evicts_full_user_stores():
    console = read("main/comms/usb_console.c")
    body = console.split("static void cmd_core_retained_witness", 1)[1].split(
        "static void cmd_contacts_clear", 1
    )[0]

    assert "existing_full_preserved" in body
    assert "core_retained_public_witness" in body
    assert "core_retained_dm_witness" in body
    assert "core_retained_contact_witness" in body
    assert "public_before.count == public_before.capacity" in body
    assert "public_before.public_count == public_before.capacity" in body
    assert "copied != expected->count" in console
    assert "dm_before.count == dm_before.capacity" in body
    assert "contact_before.count == contact_before.capacity" in body
    assert "expected->persistence_dirty" in console
    assert "expected->nvs_fallback_dirty" in console
    assert "expected->nvs_fallback_last_error != ESP_OK" in console
    assert "expected->sd_primary_required" in console
    assert '"public_evicted\\":false' in body
    assert '"dm_evicted\\":false' in body
    assert '"contact_evicted\\":false' in body
    assert "d1l_contact_store_delete" not in body
    assert "RETAINED_WITNESS_SET_NOT_FULL" in body
    assert '"witness_only\\":true' in body
    assert '"public_mutated\\":false' in body
    assert '"dm_mutated\\":false' in body
    assert '"contact_mutated\\":false' in body
    assert "d1l_message_store_append" not in body
    assert "d1l_dm_store_append" not in body
    assert "d1l_contact_store_import_uri" not in body
    assert "d1l_route_store_worker_force_flush" not in body


def test_console_labels_volatile_message_previews_explicitly():
    console = read("main/comms/usb_console.c")

    assert '"retained\\":%s,\\"volatile_preview\\":%s' in console
    assert '"volatile_preview_present\\":%s' in console
    assert '"volatile_preview_seq\\":%lu' in console
    assert "d1l_message_store_query_page_snapshot(" in console
    assert "d1l_dm_store_copy_recent_page_snapshot(" in console
    assert "d1l_dm_store_copy_thread_page_snapshot(" in console
    assert "entries[i].seq != volatile_preview_seq" in console


def test_contact_import_is_full_key_authoritative_and_truthful():
    header = read("main/mesh/contact_store.h")
    source = read("main/mesh/contact_store.c")
    console = read("main/comms/usb_console.c")
    assert "D1L_CONTACT_VERIFICATION_NONE" in header
    assert "D1L_CONTACT_VERIFICATION_SIGNED_ADVERT" in header
    assert "D1L_CONTACT_VERIFICATION_URI_IMPORT" in header
    assert "d1l_contact_store_import_uri" in header
    assert "d1l_contact_uri_parse" in source
    assert "find_unique_index_by_public_key_hex" in source
    assert "D1L_CONTACT_IMPORT_COLLISION" in source
    assert "D1L_CONTACT_IMPORT_ROLE_CONFLICT" in source
    assert "D1L_CONTACT_IMPORT_FULL" in source
    assert "oldest_evictable_placeholder_index" in source
    assert "d1l_contact_store_is_canonical" in header
    assert "d1l_contact_store_can_dm" in header
    assert "d1l_contact_store_can_admin" in header
    phase1 = read("main/ui/ui_phase1.c")
    identity_ui = read("main/ui/ui_dm_identity.c")
    assert "dm_identity_for_contact(entry, NULL).can_open_compose" in phase1
    assert "d1l_contact_store_can_dm(contact)" in identity_ui
    assert "cmd_contacts_import" in console
    assert 'ok_begin("contacts import")' in console
    assert "contacts import <meshcore-uri>" in console


def test_contact_console_json_escapes_imported_names(tmp_path):
    console = read("main/comms/usb_console.c")
    escaped_outputs = {
        "cmd_contacts": ("print_json_string(e->alias);",),
        "print_contact_export_json": ("print_json_string(e->alias);",),
        "cmd_contacts_export": ("print_json_string(contact.alias);",),
        "cmd_contacts_import": ("print_json_string(contact.alias);",),
        "cmd_contacts_add": ("print_json_string(contact.alias);",),
        "cmd_contacts_set": ("print_json_string(contact.fingerprint);",),
        "cmd_contacts_rename": ("print_json_string(contact.alias);",),
        "cmd_contacts_delete": ("print_json_string(contact.alias);",),
    }
    for symbol, required in escaped_outputs.items():
        body = static_void_body(console, symbol)
        assert '\\"alias\\":\\"%s\\"' not in body, symbol
        for token in required:
            assert token in body, symbol

    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for console JSON vectors")
    helper = c_function(console, "static void print_json_string(const char *text)")
    harness = tmp_path / "contact_json_escape_test.c"
    executable = tmp_path / (
        "contact_json_escape_test.exe" if os.name == "nt" else "contact_json_escape_test"
    )
    harness.write_text(
        "#include <stdio.h>\n"
        + helper
        + "\nint main(int argc, char **argv)\n"
          "{\n"
          "    if (argc != 2) return 2;\n"
          "    fputs(\"{\\\"alias\\\":\", stdout);\n"
          "    print_json_string(argv[1]);\n"
          "    fputs(\",\\\"safe\\\":true}\\n\", stdout);\n"
          "    return 0;\n"
          "}\n",
        encoding="utf-8",
    )
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(executable)],
        check=True,
        capture_output=True,
        text=True,
    )
    encoded_name = "A%22%2C%22x%22%3Atrue%2C%22s%22%3A%22%5CZ"
    decoded_name = unquote_plus(encoded_name)
    completed = subprocess.run(
        [str(executable), decoded_name], check=True, capture_output=True, text=True
    )
    payload = json.loads(completed.stdout)
    assert payload == {"alias": decoded_name, "safe": True}
    assert "x" not in payload
