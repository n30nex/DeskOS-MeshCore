from pathlib import Path

from scripts.smoke_d1l import SMOKE_COMMANDS


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_settings_model_defaults_and_nvs_contract():
    header = read("main/app/settings_model.h")
    source = read("main/app/settings_model.c")
    time_service = read("main/platform/time_service.c")
    time_migration = read("main/app/settings_protocol_migration.h")
    assert "D1L_SETTINGS_SCHEMA_VERSION 9U" in header
    assert "D1L_WIFI_SSID_LEN 33U" in header
    assert "D1L_WIFI_PASSWORD_LEN 65U" in header
    assert "wifi_profile_saved" in header
    assert "wifi_ssid" in header
    assert "wifi_password" in header
    assert "identity_public_key" in header
    assert "identity_private_key" in header
    assert "onboarding_complete" in header
    assert "map_location_set" in header
    assert "map_tile_provider_saved" not in header
    assert "map_tile_url_template" not in header
    assert "map_tile_attribution" not in header
    assert "map_tile_zoom" in header
    assert "D1L_MAP_TILE_DEFAULT_ZOOM 10U" in header
    assert "map_lat_e7" in header
    assert "map_lon_e7" in header
    assert "map_location_source" in header
    assert "D1L_MAP_LOCATION_SOURCE_AUTHENTICATED_COMPANION" in header
    assert "D1L_MAP_LOCATION_LAT_E7_MIN" in header
    assert "D1L_MAP_LOCATION_LON_E7_MAX" in header
    assert "d1l_settings_complete_onboarding" in header
    assert "d1l_settings_reset_onboarding" in header
    assert "d1l_settings_next_mesh_timestamp" in header
    assert "d1l_settings_load_status" in header
    assert "d1l_settings_v2_t" in source
    assert "d1l_settings_v3_t" in source
    assert "d1l_settings_v4_t" in source
    assert "d1l_settings_v5_t" in source
    assert "d1l_settings_v6_t" in source
    assert "d1l_settings_v7_t" in source
    assert "d1l_settings_v8_t" in source
    assert "migrate_v2_settings" in source
    assert "migrate_v3_settings" in source
    assert "migrate_v4_settings" in source
    assert "migrate_v5_settings" in source
    assert "migrate_v6_settings" in source
    assert "migrate_v7_settings" in source
    assert "migrate_v8_settings" in source
    assert '#include "settings_envelope.h"' in source
    assert "persist_settings_envelope" in source
    persist = source.split("static esp_err_t persist_settings_envelope", 1)[1].split(
        "static bool migrate_legacy_settings_blob", 1
    )[0]
    assert persist.count("wipe_sensitive_bytes(blob, sizeof(blob))") == 2
    assert "migrate_legacy_settings_blob" in source
    legacy_publish = source.split(
        "d1l_settings_t migrated = {0}", 1
    )[1].split("wipe_sensitive_bytes(blob, len)", 1)[0]
    assert "migrate_legacy_settings_blob(\n                            &migrated" in legacy_publish
    assert "persist_settings_envelope(\n                            handle, &migrated" in legacy_publish
    assert legacy_publish.index("persist_settings_envelope(") < legacy_publish.index(
        "s_current = migrated"
    )
    assert "wipe_sensitive_bytes(&migrated, sizeof(migrated))" in legacy_publish
    legacy = source.split("static bool migrate_legacy_settings_blob", 1)[1].split(
        "static esp_err_t quarantine_settings", 1
    )[0]
    for schema in [
        "D1L_SETTINGS_SCHEMA_VERSION",
        "8U",
        "7U",
        "6U",
        "5U",
        "4U",
        "3U",
        "2U",
    ]:
        assert f"case {schema}:" in legacy
    assert "D1L_SETTINGS_PERSISTENCE_QUARANTINED_NEWER_SCHEMA" in source
    assert "D1L_SETTINGS_PERSISTENCE_QUARANTINED_MALFORMED" in source
    assert "D1L_SETTINGS_PERSISTENCE_QUARANTINED_CHECKSUM" in source
    assert "s_persistence_write_blocked = true" in source
    assert "d1l_settings_save_wifi_profile" in header
    assert "d1l_settings_clear_wifi_profile" in header
    assert "strlen(ssid) >= D1L_WIFI_SSID_LEN" in source
    assert "settings.wifi_profile_saved = true" in source
    assert "settings.wifi_enabled = false" in source
    assert "dest->map_location_set = false" in source
    assert "dest->onboarding_complete = true" in source
    assert 'D1L_SETTINGS_NAMESPACE "d1l_settings"' in source
    assert 'D1L_SETTINGS_KEY "settings"' in source
    assert 'D1L_TIME_PROTOCOL_LEGACY_KEY "mesh_ts"' in time_migration
    assert 'D1L_TIME_PROTOCOL_HIGH_WATER_KEY "mesh_hi_v2"' in time_migration
    assert "d1l_settings_next_mesh_timestamp" not in source
    assert "d1l_settings_next_mesh_timestamp" in time_service
    assert "d1l_time_service_next_protocol_timestamp(timestamp)" in time_service
    assert "mesh_timestamp_can_fallback" not in source
    assert "next_ram_mesh_timestamp" not in source
    assert 'snprintf(settings->node_name, sizeof(settings->node_name), "D1L Desk")' in source
    assert "settings->wifi_enabled = false" in source
    assert "settings->ble_companion_enabled = false" in source
    assert "settings->observer_enabled = false" in source
    assert "settings->onboarding_complete = false" in source
    assert "settings->wifi_profile_saved = false" in source
    assert "settings->wifi_ssid[0] = '\\0'" in source
    assert "settings->wifi_password[0] = '\\0'" in source
    assert "settings->map_location_set = false" in source
    assert "settings->map_lat_e7 = 0" in source
    assert "settings->map_lon_e7 = 0" in source
    assert (
        "settings->map_location_source = D1L_MAP_LOCATION_SOURCE_UNKNOWN"
        in source
    )
    assert "settings->map_tile_zoom = D1L_MAP_TILE_DEFAULT_ZOOM" in source
    assert "settings->timezone_schema_version" in source
    assert "settings->timezone_offset_minutes = 0" in source
    assert "D1L_SETTINGS_UPDATE_TIMEZONE" in header
    assert "settings->map_tile_zoom = D1L_MAP_TILE_DEFAULT_ZOOM;" in source
    assert "settings->path_hash_bytes = 1" in source
    assert "settings->frequency_hz = D1L_RADIO_FREQ_HZ" in source
    assert "settings->tcxo_mode = D1L_TCXO_NONE" in source
    assert "settings->identity_ready = false" in source
    assert "static esp_err_t s_load_status = ESP_ERR_INVALID_STATE" in source
    load = source.split("static esp_err_t settings_load_locked(void)", 1)[1].split(
        "esp_err_t d1l_settings_load(void)", 1
    )[0]
    assert "s_load_status = ret;" in load
    save = source.split("static esp_err_t settings_save_locked", 1)[1].split(
        "static void apply_settings_update_fields", 1
    )[0]
    assert "s_load_status =" not in save
    update_wrapper = source.split("esp_err_t d1l_settings_update_fields", 1)[1].split(
        "esp_err_t d1l_settings_save_identity_if_absent", 1
    )[0]
    assert "settings_owner_take()" in update_wrapper
    assert "settings_save_locked(&updated)" in update_wrapper
    assert "settings_owner_give()" in update_wrapper
    assert "d1l_settings_current" not in header
    assert "d1l_settings_current" not in source
    assert "d1l_settings_snapshot" not in header
    assert "d1l_settings_snapshot" not in source
    assert "d1l_settings_public_snapshot" in header
    public_snapshot = source.split(
        "esp_err_t d1l_settings_public_snapshot", 1
    )[1].split("void d1l_settings_wifi_secret_wipe", 1)[0]
    assert "copy_public_settings(settings, &s_current)" in public_snapshot
    public_copy = source.split("static void copy_public_settings", 1)[1].split(
        "static void wipe_derived_identity_key", 1
    )[0]
    assert "wipe_sensitive_bytes(destination, sizeof(*destination))" in public_copy
    assert "offsetof(d1l_settings_t, wifi_password)" in public_copy
    assert "offsetof(d1l_settings_t, identity_private_key)" in public_copy
    assert "source->wifi_password" not in public_copy
    assert "source->identity_private_key" not in public_copy
    assert "d1l_settings_wifi_secret_snapshot" in header
    assert "d1l_settings_wifi_secret_wipe" in header
    assert "d1l_settings_identity_secret_snapshot" in header
    assert "d1l_settings_identity_secret_wipe" in header
    assert "D1L_SETTINGS_UPDATE_WIFI_PROFILE" not in header
    assert "D1L_SETTINGS_UPDATE_IDENTITY" not in header
    update_fields = source.split("static void apply_settings_update_fields", 1)[1].split(
        "static esp_err_t settings_replace_all", 1
    )[0]
    assert "wifi_password" not in update_fields
    assert "identity_private_key" not in update_fields
    clear_wifi = source.split(
        "esp_err_t d1l_settings_clear_wifi_profile", 1
    )[1].split("esp_err_t d1l_settings_complete_onboarding", 1)[0]
    assert "memset(settings.wifi_ssid, 0, sizeof(settings.wifi_ssid))" in clear_wifi
    assert (
        "wipe_sensitive_bytes(settings.wifi_password," in clear_wifi
    )
    save_wifi = source.split(
        "esp_err_t d1l_settings_save_wifi_profile", 1
    )[1].split("esp_err_t d1l_settings_clear_wifi_profile", 1)[0]
    ssid_wipe = save_wifi.index(
        "memset(settings.wifi_ssid, 0, sizeof(settings.wifi_ssid))"
    )
    password_wipe = save_wifi.index(
        "wipe_sensitive_bytes(settings.wifi_password,"
    )
    ssid_copy = save_wifi.index("snprintf(settings.wifi_ssid")
    password_copy = save_wifi.index("snprintf(settings.wifi_password")
    assert ssid_wipe < ssid_copy
    assert password_wipe < password_copy
    assert "zero_text_tail(settings.wifi_password," in save_wifi
    sanitize = source.split("void d1l_settings_sanitize", 1)[1].split(
        "const char *d1l_settings_role_name", 1
    )[0]
    assert "!settings->wifi_profile_saved" in sanitize
    assert "zero_text_tail(settings->wifi_ssid" in sanitize
    assert "zero_text_tail(settings->wifi_password" in sanitize


def test_console_exposes_phase2_foundation_commands():
    console = read("main/comms/usb_console.c")
    for command in [
        "settings get",
        "settings reset",
        "settings set name",
        "settings set pathhash",
        "settings set timezone",
        "settings set location",
        "settings clear location",
        "map center",
        "map center set",
        "map center clear",
        "settings onboarding status",
        "settings onboarding complete",
        "settings onboarding reset",
        "identity status",
        "radio set freq",
        "radio set bw",
        "radio set sf",
        "radio set cr",
        "radio set txpower",
        "radio set rxboost",
        "mesh advert flood",
        "wifi status",
        "wifi save",
        "wifi connect",
        "wifi clear",
        "wifi off",
        "crashlog",
        "ble status",
        'strcmp(line, "health")',
    ]:
        assert command in console

    assert "arg_len >= D1L_NODE_NAME_LEN" in console
    assert "memcpy(settings.node_name, arg, arg_len + 1U)" in console
    assert '\\"onboarding_complete\\":%s' in console
    assert "d1l_settings_complete_onboarding(arg, false, false, false)" in console
    assert "bool prompt_pending = true" in console
    assert "clearerr(stdin)" in console
    assert "vTaskDelay(pdMS_TO_TICKS(20))" in console
    assert "int ch = fgetc(stdin)" in console
    assert "line[used++] = (char)ch" in console
    assert "LINE_TOO_LONG" in console
    normal_console = console.split("void d1l_usb_console_run(void)", 1)[1].split(
        "static void factory_reset_recovery_status", 1
    )[0]
    admission = normal_console.index("d1l_usb_command_admit_in_place(")
    dispatch = normal_console.index("handle_line(&command);")
    wipe = normal_console.index(
        "wipe_console_bytes(line, sizeof(line));", dispatch
    )
    assert "const size_t received_length = used;" in normal_console
    assert "line, received_length, sizeof(line), &command" in normal_console
    assert admission < dispatch < wipe
    assert '\\"expected\\":[\\"0x20\\",\\"0x48\\"]' in console
    assert "stored_nvs_ed25519" in console
    assert "d1l_connectivity_set_wifi_enabled(false)" in console
    assert "d1l_connectivity_set_ble_enabled(false)" in console
    assert "d1l_connectivity_save_wifi_profile(ssid, password_to_save)" in console
    assert "d1l_connectivity_clear_wifi_profile()" in console
    assert "password is not printed" in console
    assert "WIFI_DISABLED" in console
    assert '\\"radio\\":{\\"frequency_hz\\":%lu' in console
    assert '\\"applied_to_radio\\":%s' in console
    assert '\\"radio_apply_pending\\":%s' in console
    assert '\\"radio_apply_error\\":\\"%s\\"' in console
    assert '\\"applied_to_radio\\":false' not in console
    assert "cmd_radio_set_txpower" in console
    assert "cmd_radio_set_rxboost" in console
    assert "settings.tx_power_dbm = (int8_t)tx_power" in console
    assert "settings.rx_boost = enabled" in console
    assert "parse_coord_e7" in console
    assert "strtod(text, &end)" in console
    assert '\\"map_location\\"' in console
    assert '\\"map_tiles\\"' in console
    assert '\\"source\\":' in console
    assert "D1L_MAP_TILE_SOURCE_ID" in console
    assert "D1L_MAP_TILE_SOURCE_URL_TEMPLATE" in console
    assert "settings->map_tile_provider_saved" not in console
    assert "provider_saved" not in console
    assert "D1L_MAP_TILE_PROVIDER_POLICY" in console
    assert "print_map_location_result" in console
    assert "d1l_app_model_set_map_location(lat_e7, lon_e7)" in console
    assert "d1l_app_model_clear_map_location()" in console
    assert "d1l_time_display_parse_timezone" in console
    assert "d1l_app_model_set_timezone_offset_minutes" in console
    assert '\\"model\\":\\"fixed_utc_offset\\"' in console
    assert '\\"auto_dst\\":false' in console


def test_v5_and_v6_migrations_preserve_wifi_location_and_identity():
    source = read("main/app/settings_model.c")

    v5 = source.split("static void migrate_v5_settings", 1)[1].split(
        "static void migrate_v6_settings", 1
    )[0]
    v6 = source.split("static void migrate_v6_settings", 1)[1].split(
        "static void migrate_v4_settings", 1
    )[0]
    for migration in (v5, v6):
        assert "dest->wifi_enabled = src->wifi_enabled" in migration
        assert "dest->wifi_profile_saved = src->wifi_profile_saved" in migration
        assert "memcpy(dest->wifi_ssid, src->wifi_ssid" in migration
        assert "memcpy(dest->wifi_password, src->wifi_password" in migration
        assert "dest->map_location_set = src->map_location_set" in migration
        assert "dest->map_lat_e7 = src->map_lat_e7" in migration
        assert "dest->map_lon_e7 = src->map_lon_e7" in migration
        assert "dest->identity_ready = src->identity_ready" in migration
        assert "memcpy(dest->identity_public_key, src->identity_public_key" in migration
        assert "memcpy(dest->identity_private_key, src->identity_private_key" in migration
        assert "dest->map_tile_zoom = D1L_MAP_TILE_DEFAULT_ZOOM" in migration

    legacy = source.split("static bool migrate_legacy_settings_blob", 1)[1].split(
        "static esp_err_t quarantine_settings", 1
    )[0]
    assert "case 5U:" in legacy
    assert "blob_length != sizeof(d1l_settings_v5_t)" in legacy
    assert "migrate_v5_settings(destination" in legacy
    assert "case 6U:" in legacy
    assert "blob_length != sizeof(d1l_settings_v6_t)" in legacy
    assert "migrate_v6_settings(destination" in legacy


def test_v7_envelope_migration_adds_safe_utc_default():
    source = read("main/app/settings_model.c")
    migration = source.split("static void migrate_v7_settings", 1)[1].split(
        "static void migrate_v6_settings", 1
    )[0]
    assert "d1l_settings_defaults(dest)" in migration
    assert "dest->schema_version = D1L_SETTINGS_SCHEMA_VERSION" in migration
    assert "d1l_settings_sanitize(dest)" in migration

    load = source.split("static esp_err_t settings_load_locked", 1)[1].split(
        "esp_err_t d1l_settings_load", 1
    )[0]
    assert "header.payload_length" in load
    assert "legacy_header.revision" in load
    assert "d1l_settings_envelope_next_revision" in load
    assert "D1L_SETTINGS_PERSISTENCE_MIGRATED_LEGACY" in load


def test_v8_migration_preserves_location_as_explicit_manual_provenance():
    source = read("main/app/settings_model.c")
    migration = source.split("static void migrate_v8_settings", 1)[1].split(
        "static void migrate_v6_settings", 1
    )[0]
    assert "src->schema_version != 8U" in migration
    assert "dest->map_location_set = src->map_location_set" in migration
    assert "D1L_MAP_LOCATION_SOURCE_MANUAL" in migration
    assert "D1L_MAP_LOCATION_SOURCE_UNKNOWN" in migration
    assert "dest->timezone_schema_version = src->timezone_schema_version" in migration
    assert "dest->timezone_offset_minutes = src->timezone_offset_minutes" in migration


def test_smoke_includes_settings_identity_and_mesh_status():
    assert "settings get" in SMOKE_COMMANDS
    assert "identity status" in SMOKE_COMMANDS
    assert "mesh status" in SMOKE_COMMANDS
    assert "map center" in SMOKE_COMMANDS
    assert "settings onboarding status" in SMOKE_COMMANDS
    assert "wifi status" in SMOKE_COMMANDS
    assert "wifi scan" in SMOKE_COMMANDS
    assert "ble status" in SMOKE_COMMANDS
