#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#include "app/settings_envelope.h"
#include "app/settings_model.h"
#include "ed_25519.h"
#include "mock_esp_nvs.h"

#define SETTINGS_NAMESPACE "d1l_settings"
#define SETTINGS_KEY "settings"
#define TEST_BLOB_MAX 4096U

typedef struct {
    uint32_t schema_version;
    char node_name[D1L_NODE_NAME_LEN];
    uint8_t role;
    bool wifi_enabled;
    bool ble_companion_enabled;
    bool observer_enabled;
    bool high_contrast;
    bool night_mode;
    bool onboarding_complete;
    bool wifi_profile_saved;
    uint8_t path_hash_bytes;
    char wifi_ssid[D1L_WIFI_SSID_LEN];
    char wifi_password[D1L_WIFI_PASSWORD_LEN];
    uint32_t frequency_hz;
    uint16_t bandwidth_tenths_khz;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    int8_t tx_power_dbm;
    bool rx_boost;
    uint8_t tcxo_mode;
    bool map_location_set;
    int32_t map_lat_e7;
    int32_t map_lon_e7;
    uint8_t map_tile_zoom;
    uint16_t timezone_schema_version;
    int16_t timezone_offset_minutes;
    bool identity_ready;
    uint8_t identity_public_key[D1L_IDENTITY_PUBLIC_KEY_LEN];
    uint8_t identity_private_key[D1L_IDENTITY_PRIVATE_KEY_LEN];
} legacy_v8_t;

_Static_assert(sizeof(legacy_v8_t) == sizeof(d1l_settings_t),
               "schema 8 and 9 must exercise equal-size migration");

static bool bytes_all_zero(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint8_t combined = 0U;
    for (size_t i = 0; i < length; ++i) {
        combined |= bytes[i];
    }
    return combined == 0U;
}

typedef struct {
    uint32_t schema_version;
    char node_name[D1L_NODE_NAME_LEN];
    uint8_t role;
    bool wifi_enabled;
    bool ble_companion_enabled;
    bool observer_enabled;
    bool high_contrast;
    bool night_mode;
    bool onboarding_complete;
    bool wifi_profile_saved;
    uint8_t path_hash_bytes;
    char wifi_ssid[D1L_WIFI_SSID_LEN];
    char wifi_password[D1L_WIFI_PASSWORD_LEN];
    uint32_t frequency_hz;
    uint16_t bandwidth_tenths_khz;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    int8_t tx_power_dbm;
    bool rx_boost;
    uint8_t tcxo_mode;
    bool map_location_set;
    int32_t map_lat_e7;
    int32_t map_lon_e7;
    uint8_t map_tile_zoom;
    bool identity_ready;
    uint8_t identity_public_key[D1L_IDENTITY_PUBLIC_KEY_LEN];
    uint8_t identity_private_key[D1L_IDENTITY_PRIVATE_KEY_LEN];
} legacy_v7_t;

typedef struct {
    uint32_t schema_version;
    char node_name[D1L_NODE_NAME_LEN];
    uint8_t role;
    bool wifi_enabled;
    bool ble_companion_enabled;
    bool observer_enabled;
    bool high_contrast;
    bool night_mode;
    bool onboarding_complete;
    bool wifi_profile_saved;
    uint8_t path_hash_bytes;
    char wifi_ssid[D1L_WIFI_SSID_LEN];
    char wifi_password[D1L_WIFI_PASSWORD_LEN];
    uint32_t frequency_hz;
    uint16_t bandwidth_tenths_khz;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    int8_t tx_power_dbm;
    bool rx_boost;
    uint8_t tcxo_mode;
    bool map_location_set;
    int32_t map_lat_e7;
    int32_t map_lon_e7;
    bool map_tile_provider_saved;
    char map_tile_url_template[D1L_MAP_TILE_URL_TEMPLATE_MAX + 1U];
    char map_tile_attribution[D1L_MAP_TILE_ATTRIBUTION_MAX + 1U];
    uint8_t map_tile_zoom;
    bool identity_ready;
    uint8_t identity_public_key[D1L_IDENTITY_PUBLIC_KEY_LEN];
    uint8_t identity_private_key[D1L_IDENTITY_PRIVATE_KEY_LEN];
} legacy_v6_t;

typedef struct {
    uint32_t schema_version;
    char node_name[D1L_NODE_NAME_LEN];
    uint8_t role;
    bool wifi_enabled;
    bool ble_companion_enabled;
    bool observer_enabled;
    bool high_contrast;
    bool night_mode;
    bool onboarding_complete;
    bool wifi_profile_saved;
    uint8_t path_hash_bytes;
    char wifi_ssid[D1L_WIFI_SSID_LEN];
    char wifi_password[D1L_WIFI_PASSWORD_LEN];
    uint32_t frequency_hz;
    uint16_t bandwidth_tenths_khz;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    int8_t tx_power_dbm;
    bool rx_boost;
    uint8_t tcxo_mode;
    bool map_location_set;
    int32_t map_lat_e7;
    int32_t map_lon_e7;
    bool identity_ready;
    uint8_t identity_public_key[D1L_IDENTITY_PUBLIC_KEY_LEN];
    uint8_t identity_private_key[D1L_IDENTITY_PRIVATE_KEY_LEN];
} legacy_v5_t;

typedef struct {
    uint32_t schema_version;
    char node_name[D1L_NODE_NAME_LEN];
    uint8_t role;
    bool wifi_enabled;
    bool ble_companion_enabled;
    bool observer_enabled;
    bool high_contrast;
    bool night_mode;
    bool onboarding_complete;
    uint8_t path_hash_bytes;
    uint32_t frequency_hz;
    uint16_t bandwidth_tenths_khz;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    int8_t tx_power_dbm;
    bool rx_boost;
    uint8_t tcxo_mode;
    bool map_location_set;
    int32_t map_lat_e7;
    int32_t map_lon_e7;
    bool identity_ready;
    uint8_t identity_public_key[D1L_IDENTITY_PUBLIC_KEY_LEN];
    uint8_t identity_private_key[D1L_IDENTITY_PRIVATE_KEY_LEN];
} legacy_v4_t;

typedef struct {
    uint32_t schema_version;
    char node_name[D1L_NODE_NAME_LEN];
    uint8_t role;
    bool wifi_enabled;
    bool ble_companion_enabled;
    bool observer_enabled;
    bool high_contrast;
    bool night_mode;
    bool onboarding_complete;
    uint8_t path_hash_bytes;
    uint32_t frequency_hz;
    uint16_t bandwidth_tenths_khz;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    int8_t tx_power_dbm;
    bool rx_boost;
    uint8_t tcxo_mode;
    bool identity_ready;
    uint8_t identity_public_key[D1L_IDENTITY_PUBLIC_KEY_LEN];
    uint8_t identity_private_key[D1L_IDENTITY_PRIVATE_KEY_LEN];
} legacy_v3_t;

typedef struct {
    uint32_t schema_version;
    char node_name[D1L_NODE_NAME_LEN];
    uint8_t role;
    bool wifi_enabled;
    bool ble_companion_enabled;
    bool observer_enabled;
    bool high_contrast;
    bool night_mode;
    uint8_t path_hash_bytes;
    uint32_t frequency_hz;
    uint16_t bandwidth_tenths_khz;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    int8_t tx_power_dbm;
    bool rx_boost;
    uint8_t tcxo_mode;
    bool identity_ready;
    uint8_t identity_public_key[D1L_IDENTITY_PUBLIC_KEY_LEN];
    uint8_t identity_private_key[D1L_IDENTITY_PRIVATE_KEY_LEN];
} legacy_v2_t;

static void assert_committed_blob_unchanged(const uint8_t *expected,
                                            size_t expected_length);

static void set_common_radio(uint32_t *frequency_hz,
                             uint16_t *bandwidth_tenths_khz,
                             uint8_t *spreading_factor,
                             uint8_t *coding_rate,
                             int8_t *tx_power_dbm)
{
    *frequency_hz = 910525000UL;
    *bandwidth_tenths_khz = 625U;
    *spreading_factor = 7U;
    *coding_rate = 5U;
    *tx_power_dbm = 20;
}

static void assert_migrated_legacy(const char *expected_name)
{
    assert(d1l_settings_load() == ESP_OK);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_MIGRATED_LEGACY);
    assert(d1l_settings_persistence_revision() == 1U);
    d1l_settings_t current = {0};
    assert(d1l_settings_public_snapshot(&current) == ESP_OK);
    assert(strcmp(current.node_name, expected_name) == 0);
    assert(current.schema_version == D1L_SETTINGS_SCHEMA_VERSION);

    uint8_t stored[TEST_BLOB_MAX] = {0};
    const size_t stored_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, stored, sizeof(stored));
    d1l_settings_envelope_header_t header = {0};
    const uint8_t *payload = NULL;
    assert(d1l_settings_envelope_validate(
               stored, stored_length, sizeof(d1l_settings_t),
               &header, &payload) == D1L_SETTINGS_ENVELOPE_VALID);
    assert(header.revision == 1U);
    assert(payload != NULL);
}

static void test_all_recognized_raw_legacy_versions_migrate(void)
{
    legacy_v7_t v7 = {
        .schema_version = 7U,
        .path_hash_bytes = 1U,
        .map_tile_zoom = D1L_MAP_TILE_DEFAULT_ZOOM,
    };
    (void)snprintf(v7.node_name, sizeof(v7.node_name), "Legacy v7");
    set_common_radio(&v7.frequency_hz, &v7.bandwidth_tenths_khz,
                     &v7.spreading_factor, &v7.coding_rate,
                     &v7.tx_power_dbm);
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              &v7, sizeof(v7)));
    assert_migrated_legacy("Legacy v7");
    d1l_settings_t migrated_v7 = {0};
    assert(d1l_settings_public_snapshot(&migrated_v7) == ESP_OK);
    assert(migrated_v7.timezone_schema_version ==
           D1L_TIMEZONE_SETTING_SCHEMA_VERSION);
    assert(migrated_v7.timezone_offset_minutes == 0);

    legacy_v6_t v6 = {.schema_version = 6U, .path_hash_bytes = 2U};
    (void)snprintf(v6.node_name, sizeof(v6.node_name), "Legacy v6");
    set_common_radio(&v6.frequency_hz, &v6.bandwidth_tenths_khz,
                     &v6.spreading_factor, &v6.coding_rate,
                     &v6.tx_power_dbm);
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              &v6, sizeof(v6)));
    assert_migrated_legacy("Legacy v6");

    legacy_v5_t v5 = {.schema_version = 5U, .path_hash_bytes = 2U};
    (void)snprintf(v5.node_name, sizeof(v5.node_name), "Legacy v5");
    set_common_radio(&v5.frequency_hz, &v5.bandwidth_tenths_khz,
                     &v5.spreading_factor, &v5.coding_rate,
                     &v5.tx_power_dbm);
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              &v5, sizeof(v5)));
    assert_migrated_legacy("Legacy v5");

    legacy_v4_t v4 = {.schema_version = 4U, .path_hash_bytes = 2U};
    (void)snprintf(v4.node_name, sizeof(v4.node_name), "Legacy v4");
    set_common_radio(&v4.frequency_hz, &v4.bandwidth_tenths_khz,
                     &v4.spreading_factor, &v4.coding_rate,
                     &v4.tx_power_dbm);
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              &v4, sizeof(v4)));
    assert_migrated_legacy("Legacy v4");

    legacy_v3_t v3 = {.schema_version = 3U, .path_hash_bytes = 2U};
    (void)snprintf(v3.node_name, sizeof(v3.node_name), "Legacy v3");
    set_common_radio(&v3.frequency_hz, &v3.bandwidth_tenths_khz,
                     &v3.spreading_factor, &v3.coding_rate,
                     &v3.tx_power_dbm);
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              &v3, sizeof(v3)));
    assert_migrated_legacy("Legacy v3");

    legacy_v2_t v2 = {.schema_version = 2U, .path_hash_bytes = 2U};
    (void)snprintf(v2.node_name, sizeof(v2.node_name), "Legacy v2");
    set_common_radio(&v2.frequency_hz, &v2.bandwidth_tenths_khz,
                     &v2.spreading_factor, &v2.coding_rate,
                     &v2.tx_power_dbm);
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              &v2, sizeof(v2)));
    assert_migrated_legacy("Legacy v2");
    d1l_settings_t current = {0};
    assert(d1l_settings_public_snapshot(&current) == ESP_OK);
    assert(current.onboarding_complete);
}

static void test_v7_envelope_migrates_revision_and_recovers_text(void)
{
    legacy_v7_t legacy = {
        .schema_version = 7U,
        .path_hash_bytes = 1U,
        .map_tile_zoom = D1L_MAP_TILE_DEFAULT_ZOOM,
    };
    memset(legacy.node_name, 'X', sizeof(legacy.node_name));
    set_common_radio(&legacy.frequency_hz, &legacy.bandwidth_tenths_khz,
                     &legacy.spreading_factor, &legacy.coding_rate,
                     &legacy.tx_power_dbm);
    uint8_t blob[TEST_BLOB_MAX] = {0};
    size_t length = 0U;
    assert(d1l_settings_envelope_build(
        blob, sizeof(blob), &legacy, sizeof(legacy), 41U, &length));
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY, blob, length));
    assert(d1l_settings_load() == ESP_OK);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_MIGRATED_LEGACY);
    assert(d1l_settings_persistence_revision() == 42U);
    d1l_settings_t current = {0};
    assert(d1l_settings_public_snapshot(&current) == ESP_OK);
    assert(current.node_name[D1L_NODE_NAME_LEN - 1U] == '\0');
    assert(current.timezone_schema_version ==
           D1L_TIMEZONE_SETTING_SCHEMA_VERSION);
    assert(current.timezone_offset_minutes == 0);

    uint8_t stored[TEST_BLOB_MAX] = {0};
    const size_t stored_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, stored, sizeof(stored));
    d1l_settings_envelope_header_t header = {0};
    assert(d1l_settings_envelope_validate(
               stored, stored_length, sizeof(d1l_settings_t),
               &header, NULL) == D1L_SETTINGS_ENVELOPE_VALID);
    assert(header.revision == 42U);
}

static void test_equal_size_v8_envelope_migrates_and_preserves_identity(void)
{
    legacy_v8_t legacy = {
        .schema_version = 8U,
        .path_hash_bytes = 1U,
        .map_location_set = true,
        .map_lat_e7 = 436532000,
        .map_lon_e7 = -793832000,
        .map_tile_zoom = D1L_MAP_TILE_DEFAULT_ZOOM,
        .timezone_schema_version = D1L_TIMEZONE_SETTING_SCHEMA_VERSION,
        .timezone_offset_minutes = -240,
    };
    (void)snprintf(legacy.node_name, sizeof(legacy.node_name), "D1L Desk");
    set_common_radio(&legacy.frequency_hz, &legacy.bandwidth_tenths_khz,
                     &legacy.spreading_factor, &legacy.coding_rate,
                     &legacy.tx_power_dbm);
    uint8_t seed[D1L_IDENTITY_PUBLIC_KEY_LEN] = {0};
    for (size_t i = 0; i < sizeof(seed); ++i) {
        seed[i] = (uint8_t)(i + 31U);
    }
    ed25519_create_keypair(legacy.identity_public_key,
                           legacy.identity_private_key, seed);
    legacy.identity_ready = true;

    uint8_t blob[TEST_BLOB_MAX] = {0};
    size_t length = 0U;
    assert(d1l_settings_envelope_build(
        blob, sizeof(blob), &legacy, sizeof(legacy), 21U, &length));
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY, blob, length));
    assert(d1l_settings_load() == ESP_OK);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_MIGRATED_LEGACY);
    assert(d1l_settings_persistence_revision() == 22U);

    d1l_settings_t current = {0};
    assert(d1l_settings_public_snapshot(&current) == ESP_OK);
    assert(current.schema_version == D1L_SETTINGS_SCHEMA_VERSION);
    assert(strcmp(current.node_name, "D1L Desk") == 0);
    assert(current.map_location_source == D1L_MAP_LOCATION_SOURCE_MANUAL);
    assert(current.timezone_offset_minutes == -240);
    assert(current.identity_ready);
    assert(memcmp(current.identity_public_key, legacy.identity_public_key,
                  sizeof(current.identity_public_key)) == 0);
    assert(bytes_all_zero(current.identity_private_key,
                          sizeof(current.identity_private_key)));

    d1l_settings_identity_secret_t identity = {0};
    assert(d1l_settings_identity_secret_snapshot(&identity) == ESP_OK);
    assert(memcmp(identity.identity_private_key, legacy.identity_private_key,
                  sizeof(identity.identity_private_key)) == 0);
    d1l_settings_identity_secret_wipe(&identity);

    uint8_t stored[TEST_BLOB_MAX] = {0};
    const size_t stored_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, stored, sizeof(stored));
    d1l_settings_envelope_header_t header = {0};
    const uint8_t *payload = NULL;
    assert(d1l_settings_envelope_validate(
               stored, stored_length, sizeof(d1l_settings_t),
               &header, &payload) == D1L_SETTINGS_ENVELOPE_VALID);
    assert(header.revision == 22U);
    uint32_t stored_schema = 0U;
    memcpy(&stored_schema, payload, sizeof(stored_schema));
    assert(stored_schema == D1L_SETTINGS_SCHEMA_VERSION);
}

static void test_invalid_timezone_recovers_to_utc_without_boot_loop(void)
{
    d1l_settings_t settings = {0};
    d1l_settings_defaults(&settings);
    settings.timezone_schema_version =
        D1L_TIMEZONE_SETTING_SCHEMA_VERSION + 1U;
    settings.timezone_offset_minutes = 841;
    uint8_t blob[TEST_BLOB_MAX] = {0};
    size_t length = 0U;
    assert(d1l_settings_envelope_build(
        blob, sizeof(blob), &settings, sizeof(settings), 5U, &length));
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY, blob, length));
    assert(d1l_settings_load() == ESP_OK);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_READY);
    d1l_settings_t recovered = {0};
    assert(d1l_settings_public_snapshot(&recovered) == ESP_OK);
    assert(recovered.timezone_schema_version ==
           D1L_TIMEZONE_SETTING_SCHEMA_VERSION);
    assert(recovered.timezone_offset_minutes == 0);

    recovered.timezone_schema_version =
        D1L_TIMEZONE_SETTING_SCHEMA_VERSION;
    recovered.timezone_offset_minutes = -240;
    assert(d1l_settings_update_fields(
               &recovered, D1L_SETTINGS_UPDATE_TIMEZONE) == ESP_OK);
    assert(d1l_settings_public_snapshot(&recovered) == ESP_OK);
    assert(recovered.timezone_offset_minutes == -240);
    assert(d1l_settings_reset() == ESP_OK);
    assert(d1l_settings_public_snapshot(&recovered) == ESP_OK);
    assert(recovered.timezone_offset_minutes == 0);
}

static void test_failed_legacy_rewrite_leaves_raw_blob_intact(void)
{
    d1l_settings_t legacy = {0};
    d1l_settings_defaults(&legacy);
    (void)snprintf(legacy.node_name, sizeof(legacy.node_name),
                   "Legacy retry");
    legacy.wifi_profile_saved = true;
    (void)snprintf(legacy.wifi_ssid, sizeof(legacy.wifi_ssid),
                   "Legacy secret network");
    (void)snprintf(legacy.wifi_password, sizeof(legacy.wifi_password),
                   "legacy secret password");
    uint8_t seed[D1L_IDENTITY_PUBLIC_KEY_LEN] = {0};
    for (size_t i = 0; i < sizeof(seed); ++i) {
        seed[i] = (uint8_t)(i + 17U);
    }
    ed25519_create_keypair(legacy.identity_public_key,
                           legacy.identity_private_key, seed);
    legacy.identity_ready = true;
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              &legacy, sizeof(legacy)));
    mock_nvs_fail_next_set(ESP_FAIL);
    assert(d1l_settings_load() == ESP_FAIL);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_IO_ERROR);
    d1l_settings_t public_snapshot;
    memset(&public_snapshot, 0xa5, sizeof(public_snapshot));
    assert(d1l_settings_public_snapshot(&public_snapshot) == ESP_FAIL);
    assert(!public_snapshot.wifi_profile_saved);
    assert(!public_snapshot.identity_ready);
    assert(bytes_all_zero(public_snapshot.wifi_password,
                          sizeof(public_snapshot.wifi_password)));
    assert(bytes_all_zero(public_snapshot.identity_private_key,
                          sizeof(public_snapshot.identity_private_key)));
    d1l_settings_wifi_secret_t wifi_secret;
    memset(&wifi_secret, 0xa5, sizeof(wifi_secret));
    assert(d1l_settings_wifi_secret_snapshot(&wifi_secret) == ESP_FAIL);
    assert(bytes_all_zero(&wifi_secret, sizeof(wifi_secret)));
    d1l_settings_identity_secret_t identity_secret;
    memset(&identity_secret, 0xa5, sizeof(identity_secret));
    assert(d1l_settings_identity_secret_snapshot(&identity_secret) == ESP_FAIL);
    assert(bytes_all_zero(&identity_secret, sizeof(identity_secret)));
    assert_committed_blob_unchanged((const uint8_t *)&legacy,
                                    sizeof(legacy));
    assert(d1l_settings_update_fields(
               &legacy, D1L_SETTINGS_UPDATE_NODE_NAME) ==
           ESP_ERR_INVALID_STATE);
    assert_committed_blob_unchanged((const uint8_t *)&legacy,
                                    sizeof(legacy));

    legacy_v7_t enveloped = {
        .schema_version = 7U,
        .path_hash_bytes = 1U,
        .map_tile_zoom = D1L_MAP_TILE_DEFAULT_ZOOM,
    };
    (void)snprintf(enveloped.node_name, sizeof(enveloped.node_name),
                   "Envelope retry");
    set_common_radio(&enveloped.frequency_hz,
                     &enveloped.bandwidth_tenths_khz,
                     &enveloped.spreading_factor,
                     &enveloped.coding_rate, &enveloped.tx_power_dbm);
    uint8_t envelope[TEST_BLOB_MAX] = {0};
    size_t envelope_length = 0U;
    assert(d1l_settings_envelope_build(
        envelope, sizeof(envelope), &enveloped, sizeof(enveloped), 9U,
        &envelope_length));
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              envelope, envelope_length));
    mock_nvs_fail_next_set(ESP_FAIL);
    assert(d1l_settings_load() == ESP_FAIL);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_IO_ERROR);
    assert_committed_blob_unchanged(envelope, envelope_length);
    assert(d1l_settings_update_fields(
               &legacy, D1L_SETTINGS_UPDATE_NODE_NAME) ==
           ESP_ERR_INVALID_STATE);
    assert_committed_blob_unchanged(envelope, envelope_length);
    memset(seed, 0, sizeof(seed));
    memset(&legacy, 0, sizeof(legacy));
}

static size_t build_current_envelope(uint8_t blob[TEST_BLOB_MAX],
                                     uint32_t revision)
{
    d1l_settings_t settings = {0};
    d1l_settings_defaults(&settings);
    size_t length = 0U;
    assert(d1l_settings_envelope_build(
        blob, TEST_BLOB_MAX, &settings, sizeof(settings), revision, &length));
    return length;
}

static void assert_committed_blob_unchanged(const uint8_t *expected,
                                            size_t expected_length)
{
    uint8_t actual[TEST_BLOB_MAX] = {0};
    const size_t actual_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, actual, sizeof(actual));
    assert(actual_length == expected_length);
    assert(memcmp(actual, expected, expected_length) == 0);
}

static void test_unknown_newer_envelope_is_preserved_and_write_blocked(void)
{
    uint8_t blob[TEST_BLOB_MAX] = {0};
    const size_t length = build_current_envelope(blob, 4U);
    d1l_settings_envelope_header_t header = {0};
    memcpy(&header, blob, sizeof(header));
    header.schema_version = D1L_SETTINGS_ENVELOPE_SCHEMA_VERSION + 1U;
    memcpy(blob, &header, sizeof(header));

    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY, blob, length));
    assert(d1l_settings_load() == ESP_ERR_NOT_SUPPORTED);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_QUARANTINED_NEWER_SCHEMA);
    assert(d1l_settings_persistence_revision() == 0U);
    d1l_settings_t defaults = {0};
    d1l_settings_defaults(&defaults);
    assert(d1l_settings_update_fields(
               &defaults, D1L_SETTINGS_UPDATE_NODE_NAME) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(d1l_settings_save_status() == ESP_ERR_NOT_SUPPORTED);
    assert_committed_blob_unchanged(blob, length);

    d1l_settings_t future = {0};
    d1l_settings_defaults(&future);
    future.schema_version = D1L_SETTINGS_SCHEMA_VERSION + 1U;
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              &future, sizeof(future)));
    assert(d1l_settings_load() == ESP_ERR_NOT_SUPPORTED);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_QUARANTINED_NEWER_SCHEMA);
    assert_committed_blob_unchanged((const uint8_t *)&future,
                                    sizeof(future));

    memset(blob, 0, sizeof(blob));
    size_t future_envelope_length = 0U;
    assert(d1l_settings_envelope_build(
        blob, sizeof(blob), &future, sizeof(future), 5U,
        &future_envelope_length));
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              blob, future_envelope_length));
    assert(d1l_settings_load() == ESP_ERR_NOT_SUPPORTED);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_QUARANTINED_NEWER_SCHEMA);
    assert_committed_blob_unchanged(blob, future_envelope_length);

    uint8_t extended_payload[sizeof(future) + 8U] = {0};
    memcpy(extended_payload, &future, sizeof(future));
    memset(blob, 0, sizeof(blob));
    assert(d1l_settings_envelope_build(
        blob, sizeof(blob), extended_payload, sizeof(extended_payload), 6U,
        &future_envelope_length));
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              blob, future_envelope_length));
    assert(d1l_settings_load() == ESP_ERR_NOT_SUPPORTED);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_QUARANTINED_NEWER_SCHEMA);
    assert_committed_blob_unchanged(blob, future_envelope_length);
}

static void test_corruption_and_malformed_length_are_preserved(void)
{
    uint8_t blob[TEST_BLOB_MAX] = {0};
    size_t length = build_current_envelope(blob, 8U);
    blob[sizeof(d1l_settings_envelope_header_t) + 11U] ^= 0x01U;
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY, blob, length));
    assert(d1l_settings_load() == ESP_FAIL);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_QUARANTINED_CHECKSUM);
    d1l_settings_t defaults = {0};
    d1l_settings_defaults(&defaults);
    assert(d1l_settings_update_fields(
               &defaults, D1L_SETTINGS_UPDATE_NODE_NAME) == ESP_FAIL);
    assert_committed_blob_unchanged(blob, length);

    length = build_current_envelope(blob, 8U);
    d1l_settings_envelope_header_t header = {0};
    memcpy(&header, blob, sizeof(header));
    header.payload_length--;
    memcpy(blob, &header, sizeof(header));
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY, blob, length));
    assert(d1l_settings_load() == ESP_ERR_INVALID_SIZE);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_QUARANTINED_MALFORMED);
    assert(d1l_settings_update_fields(
               &defaults, D1L_SETTINGS_UPDATE_NODE_NAME) ==
           ESP_ERR_INVALID_SIZE);
    assert_committed_blob_unchanged(blob, length);
}

static void test_monotonic_save_and_revision_saturation(void)
{
    mock_nvs_reset();
    assert(d1l_settings_load() == ESP_OK);
    assert(d1l_settings_persistence_revision() == 1U);
    d1l_settings_t settings = {0};
    assert(d1l_settings_public_snapshot(&settings) == ESP_OK);
    (void)snprintf(settings.node_name, sizeof(settings.node_name), "Revision two");
    assert(d1l_settings_update_fields(
               &settings, D1L_SETTINGS_UPDATE_NODE_NAME) == ESP_OK);
    assert(d1l_settings_persistence_revision() == 2U);
    assert(d1l_settings_save_status() == ESP_OK);
    uint8_t stored[TEST_BLOB_MAX] = {0};
    const size_t stored_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, stored, sizeof(stored));
    d1l_settings_envelope_header_t stored_header = {0};
    const uint8_t *stored_payload = NULL;
    assert(d1l_settings_envelope_validate(
               stored, stored_length, sizeof(d1l_settings_t),
               &stored_header, &stored_payload) ==
           D1L_SETTINGS_ENVELOPE_VALID);
    assert(stored_header.revision == 2U);
    d1l_settings_t stored_settings = {0};
    memcpy(&stored_settings, stored_payload, sizeof(stored_settings));
    assert(strcmp(stored_settings.node_name, "Revision two") == 0);

    uint8_t blob[TEST_BLOB_MAX] = {0};
    const size_t length = build_current_envelope(blob, UINT32_MAX);
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY, blob, length));
    assert(d1l_settings_load() == ESP_OK);
    assert(d1l_settings_public_snapshot(&settings) == ESP_OK);
    settings.night_mode = true;
    assert(d1l_settings_update_fields(
               &settings, D1L_SETTINGS_UPDATE_NIGHT_MODE) ==
           ESP_ERR_INVALID_STATE);
    assert(d1l_settings_persistence_state() ==
           D1L_SETTINGS_PERSISTENCE_REVISION_SATURATED);
    assert_committed_blob_unchanged(blob, length);
}

static void test_secret_update_bits_are_rejected(void)
{
    mock_nvs_reset();
    assert(d1l_settings_load() == ESP_OK);
    assert(d1l_settings_save_wifi_profile(
               "Protected network", "protected password") == ESP_OK);
    uint8_t committed[TEST_BLOB_MAX] = {0};
    const size_t committed_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, committed, sizeof(committed));
    assert(committed_length > 0U);

    d1l_settings_t attempted = {0};
    assert(d1l_settings_public_snapshot(&attempted) == ESP_OK);
    memset(attempted.wifi_password, 'W', sizeof(attempted.wifi_password));
    memset(attempted.identity_private_key, 'I',
           sizeof(attempted.identity_private_key));
    assert(d1l_settings_update_fields(
               &attempted, (d1l_settings_update_mask_t)(1UL << 8)) ==
           ESP_ERR_INVALID_ARG);
    assert_committed_blob_unchanged(committed, committed_length);
    assert(d1l_settings_update_fields(
               &attempted, (d1l_settings_update_mask_t)(1UL << 18)) ==
           ESP_ERR_INVALID_ARG);
    assert_committed_blob_unchanged(committed, committed_length);
    memset(&attempted, 0, sizeof(attempted));
}

static void test_wifi_secret_tail_canonicalization(void)
{
    d1l_settings_t tainted = {0};
    d1l_settings_defaults(&tainted);
    tainted.wifi_profile_saved = true;
    memset(tainted.wifi_ssid, 'S', sizeof(tainted.wifi_ssid));
    memcpy(tainted.wifi_ssid, "Visible network", sizeof("Visible network"));
    memset(tainted.wifi_password, 'P', sizeof(tainted.wifi_password));
    memcpy(tainted.wifi_password, "visible password",
           sizeof("visible password"));

    uint8_t blob[TEST_BLOB_MAX] = {0};
    size_t blob_length = 0U;
    assert(d1l_settings_envelope_build(
        blob, sizeof(blob), &tainted, sizeof(tainted), 9U, &blob_length));
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              blob, blob_length));
    assert(d1l_settings_load() == ESP_OK);

    d1l_settings_t public_snapshot = {0};
    assert(d1l_settings_public_snapshot(&public_snapshot) == ESP_OK);
    assert(strcmp(public_snapshot.wifi_ssid, "Visible network") == 0);
    assert(bytes_all_zero(public_snapshot.wifi_password,
                          sizeof(public_snapshot.wifi_password)));
    d1l_settings_wifi_secret_t wifi_secret = {0};
    assert(d1l_settings_wifi_secret_snapshot(&wifi_secret) == ESP_OK);
    assert(strcmp(wifi_secret.wifi_ssid, "Visible network") == 0);
    assert(strcmp(wifi_secret.wifi_password, "visible password") == 0);
    assert(bytes_all_zero(
        &wifi_secret.wifi_ssid[strlen(wifi_secret.wifi_ssid) + 1U],
        sizeof(wifi_secret.wifi_ssid) - strlen(wifi_secret.wifi_ssid) - 1U));
    assert(bytes_all_zero(
        &wifi_secret.wifi_password[strlen(wifi_secret.wifi_password) + 1U],
        sizeof(wifi_secret.wifi_password) -
            strlen(wifi_secret.wifi_password) - 1U));
    d1l_settings_wifi_secret_wipe(&wifi_secret);

    public_snapshot.night_mode = true;
    assert(d1l_settings_update_fields(
               &public_snapshot, D1L_SETTINGS_UPDATE_NIGHT_MODE) == ESP_OK);
    memset(blob, 0, sizeof(blob));
    blob_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, blob, sizeof(blob));
    d1l_settings_envelope_header_t header = {0};
    const uint8_t *payload = NULL;
    assert(d1l_settings_envelope_validate(
               blob, blob_length, sizeof(d1l_settings_t),
               &header, &payload) == D1L_SETTINGS_ENVELOPE_VALID);
    assert(header.revision == 10U);
    d1l_settings_t persisted = {0};
    memcpy(&persisted, payload, sizeof(persisted));
    assert(strcmp(persisted.wifi_ssid, "Visible network") == 0);
    assert(strcmp(persisted.wifi_password, "visible password") == 0);
    assert(bytes_all_zero(
        &persisted.wifi_ssid[strlen(persisted.wifi_ssid) + 1U],
        sizeof(persisted.wifi_ssid) - strlen(persisted.wifi_ssid) - 1U));
    assert(bytes_all_zero(
        &persisted.wifi_password[strlen(persisted.wifi_password) + 1U],
        sizeof(persisted.wifi_password) -
            strlen(persisted.wifi_password) - 1U));

    legacy_v6_t legacy = {.schema_version = 6U, .path_hash_bytes = 1U};
    (void)snprintf(legacy.node_name, sizeof(legacy.node_name),
                   "Legacy stale Wi-Fi");
    set_common_radio(&legacy.frequency_hz, &legacy.bandwidth_tenths_khz,
                     &legacy.spreading_factor, &legacy.coding_rate,
                     &legacy.tx_power_dbm);
    legacy.wifi_profile_saved = false;
    memset(legacy.wifi_ssid, 'L', sizeof(legacy.wifi_ssid));
    memset(legacy.wifi_password, 'Q', sizeof(legacy.wifi_password));
    mock_nvs_reset();
    assert(mock_nvs_seed_blob(SETTINGS_NAMESPACE, SETTINGS_KEY,
                              &legacy, sizeof(legacy)));
    assert_migrated_legacy("Legacy stale Wi-Fi");
    assert(d1l_settings_wifi_secret_snapshot(&wifi_secret) == ESP_OK);
    assert(!wifi_secret.wifi_profile_saved);
    assert(bytes_all_zero(wifi_secret.wifi_ssid,
                          sizeof(wifi_secret.wifi_ssid)));
    assert(bytes_all_zero(wifi_secret.wifi_password,
                          sizeof(wifi_secret.wifi_password)));
    d1l_settings_wifi_secret_wipe(&wifi_secret);

    memset(blob, 0, sizeof(blob));
    blob_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, blob, sizeof(blob));
    payload = NULL;
    assert(d1l_settings_envelope_validate(
               blob, blob_length, sizeof(d1l_settings_t),
               NULL, &payload) == D1L_SETTINGS_ENVELOPE_VALID);
    memset(&persisted, 0, sizeof(persisted));
    memcpy(&persisted, payload, sizeof(persisted));
    assert(!persisted.wifi_profile_saved);
    assert(bytes_all_zero(persisted.wifi_ssid,
                          sizeof(persisted.wifi_ssid)));
    assert(bytes_all_zero(persisted.wifi_password,
                          sizeof(persisted.wifi_password)));
}

static void test_public_snapshot_redacts_typed_secrets(void)
{
    mock_nvs_reset();
    assert(d1l_settings_load() == ESP_OK);
    assert(d1l_settings_save_wifi_profile(
               "Secret network", "correct horse battery staple") == ESP_OK);

    uint8_t seed[D1L_IDENTITY_PUBLIC_KEY_LEN] = {0};
    for (size_t i = 0; i < sizeof(seed); ++i) {
        seed[i] = (uint8_t)(i + 1U);
    }
    uint8_t public_key[D1L_IDENTITY_PUBLIC_KEY_LEN] = {0};
    uint8_t private_key[D1L_IDENTITY_PRIVATE_KEY_LEN] = {0};
    ed25519_create_keypair(public_key, private_key, seed);
    assert(d1l_settings_save_identity_if_absent(
               public_key, private_key) == ESP_OK);

    d1l_settings_t public_snapshot = {0};
    assert(d1l_settings_public_snapshot(&public_snapshot) == ESP_OK);
    assert(strcmp(public_snapshot.wifi_ssid, "Secret network") == 0);
    assert(public_snapshot.wifi_profile_saved);
    assert(d1l_settings_wifi_password_saved());
    assert(bytes_all_zero(public_snapshot.wifi_password,
                          sizeof(public_snapshot.wifi_password)));
    assert(public_snapshot.identity_ready);
    assert(memcmp(public_snapshot.identity_public_key, public_key,
                  sizeof(public_key)) == 0);
    assert(bytes_all_zero(public_snapshot.identity_private_key,
                          sizeof(public_snapshot.identity_private_key)));
    assert(d1l_settings_persisted_identity_state() ==
           D1L_IDENTITY_STATE_CONSISTENT);

    d1l_settings_t timezone_update = {
        .timezone_schema_version =
            D1L_TIMEZONE_SETTING_SCHEMA_VERSION,
        .timezone_offset_minutes = 345,
    };
    assert(d1l_settings_update_fields(
               &timezone_update, D1L_SETTINGS_UPDATE_TIMEZONE) == ESP_OK);
    d1l_settings_wifi_secret_t timezone_wifi = {0};
    assert(d1l_settings_wifi_secret_snapshot(&timezone_wifi) == ESP_OK);
    assert(strcmp(timezone_wifi.wifi_password,
                  "correct horse battery staple") == 0);
    d1l_settings_wifi_secret_wipe(&timezone_wifi);
    d1l_settings_identity_secret_t timezone_identity = {0};
    assert(d1l_settings_identity_secret_snapshot(&timezone_identity) == ESP_OK);
    assert(memcmp(timezone_identity.identity_private_key, private_key,
                  sizeof(private_key)) == 0);
    d1l_settings_identity_secret_wipe(&timezone_identity);
    assert(d1l_settings_public_snapshot(&public_snapshot) == ESP_OK);
    assert(public_snapshot.timezone_offset_minutes == 345);

    d1l_settings_wifi_secret_t wifi_secret = {0};
    assert(d1l_settings_wifi_secret_snapshot(&wifi_secret) == ESP_OK);
    assert(wifi_secret.wifi_enabled == public_snapshot.wifi_enabled);
    assert(wifi_secret.wifi_profile_saved);
    assert(strcmp(wifi_secret.wifi_ssid, "Secret network") == 0);
    assert(strcmp(wifi_secret.wifi_password,
                  "correct horse battery staple") == 0);
    d1l_settings_wifi_secret_wipe(&wifi_secret);
    assert(bytes_all_zero(&wifi_secret, sizeof(wifi_secret)));

    d1l_settings_identity_secret_t identity_secret = {0};
    assert(d1l_settings_identity_secret_snapshot(&identity_secret) == ESP_OK);
    assert(identity_secret.identity_ready);
    assert(memcmp(identity_secret.identity_public_key, public_key,
                  sizeof(public_key)) == 0);
    assert(memcmp(identity_secret.identity_private_key, private_key,
                  sizeof(private_key)) == 0);
    d1l_settings_identity_secret_wipe(&identity_secret);
    assert(bytes_all_zero(&identity_secret, sizeof(identity_secret)));

    uint8_t stored[TEST_BLOB_MAX] = {0};
    const size_t stored_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, stored, sizeof(stored));
    const uint8_t *payload = NULL;
    assert(d1l_settings_envelope_validate(
               stored, stored_length, sizeof(d1l_settings_t),
               NULL, &payload) == D1L_SETTINGS_ENVELOPE_VALID);
    d1l_settings_t persisted = {0};
    memcpy(&persisted, payload, sizeof(persisted));
    assert(strcmp(persisted.wifi_password,
                  "correct horse battery staple") == 0);
    assert(memcmp(persisted.identity_private_key, private_key,
                  sizeof(private_key)) == 0);

    assert(d1l_settings_save_wifi_profile("New network", "short") == ESP_OK);
    assert(d1l_settings_wifi_secret_snapshot(&wifi_secret) == ESP_OK);
    assert(strcmp(wifi_secret.wifi_ssid, "New network") == 0);
    assert(strcmp(wifi_secret.wifi_password, "short") == 0);
    assert(bytes_all_zero(
        &wifi_secret.wifi_password[strlen(wifi_secret.wifi_password) + 1U],
        sizeof(wifi_secret.wifi_password) -
            strlen(wifi_secret.wifi_password) - 1U));
    d1l_settings_wifi_secret_wipe(&wifi_secret);

    memset(stored, 0, sizeof(stored));
    const size_t replacement_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, stored, sizeof(stored));
    payload = NULL;
    assert(d1l_settings_envelope_validate(
               stored, replacement_length, sizeof(d1l_settings_t),
               NULL, &payload) == D1L_SETTINGS_ENVELOPE_VALID);
    memset(&persisted, 0, sizeof(persisted));
    memcpy(&persisted, payload, sizeof(persisted));
    assert(strcmp(persisted.wifi_ssid, "New network") == 0);
    assert(strcmp(persisted.wifi_password, "short") == 0);
    assert(bytes_all_zero(
        &persisted.wifi_ssid[strlen(persisted.wifi_ssid) + 1U],
        sizeof(persisted.wifi_ssid) - strlen(persisted.wifi_ssid) - 1U));
    assert(bytes_all_zero(
        &persisted.wifi_password[strlen(persisted.wifi_password) + 1U],
        sizeof(persisted.wifi_password) -
            strlen(persisted.wifi_password) - 1U));

    assert(d1l_settings_save_wifi_profile("Renamed network", NULL) == ESP_OK);
    assert(d1l_settings_wifi_secret_snapshot(&wifi_secret) == ESP_OK);
    assert(wifi_secret.wifi_profile_saved);
    assert(strcmp(wifi_secret.wifi_ssid, "Renamed network") == 0);
    assert(strcmp(wifi_secret.wifi_password, "short") == 0);
    assert(bytes_all_zero(
        &wifi_secret.wifi_password[strlen(wifi_secret.wifi_password) + 1U],
        sizeof(wifi_secret.wifi_password) -
            strlen(wifi_secret.wifi_password) - 1U));
    d1l_settings_wifi_secret_wipe(&wifi_secret);

    memset(stored, 0, sizeof(stored));
    const size_t preserved_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, stored, sizeof(stored));
    payload = NULL;
    assert(d1l_settings_envelope_validate(
               stored, preserved_length, sizeof(d1l_settings_t),
               NULL, &payload) == D1L_SETTINGS_ENVELOPE_VALID);
    memset(&persisted, 0, sizeof(persisted));
    memcpy(&persisted, payload, sizeof(persisted));
    assert(strcmp(persisted.wifi_ssid, "Renamed network") == 0);
    assert(strcmp(persisted.wifi_password, "short") == 0);
    assert(bytes_all_zero(
        &persisted.wifi_password[strlen(persisted.wifi_password) + 1U],
        sizeof(persisted.wifi_password) -
            strlen(persisted.wifi_password) - 1U));

    assert(d1l_settings_save_wifi_profile("Open network", "") == ESP_OK);
    assert(d1l_settings_wifi_secret_snapshot(&wifi_secret) == ESP_OK);
    assert(wifi_secret.wifi_profile_saved);
    assert(strcmp(wifi_secret.wifi_ssid, "Open network") == 0);
    assert(bytes_all_zero(wifi_secret.wifi_password,
                          sizeof(wifi_secret.wifi_password)));
    d1l_settings_wifi_secret_wipe(&wifi_secret);

    memset(stored, 0, sizeof(stored));
    const size_t open_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, stored, sizeof(stored));
    payload = NULL;
    assert(d1l_settings_envelope_validate(
               stored, open_length, sizeof(d1l_settings_t),
               NULL, &payload) == D1L_SETTINGS_ENVELOPE_VALID);
    memset(&persisted, 0, sizeof(persisted));
    memcpy(&persisted, payload, sizeof(persisted));
    assert(strcmp(persisted.wifi_ssid, "Open network") == 0);
    assert(bytes_all_zero(persisted.wifi_password,
                          sizeof(persisted.wifi_password)));

    assert(d1l_settings_clear_wifi_profile() == ESP_OK);
    assert(d1l_settings_wifi_secret_snapshot(&wifi_secret) == ESP_OK);
    assert(!wifi_secret.wifi_profile_saved);
    assert(bytes_all_zero(wifi_secret.wifi_ssid,
                          sizeof(wifi_secret.wifi_ssid)));
    assert(bytes_all_zero(wifi_secret.wifi_password,
                          sizeof(wifi_secret.wifi_password)));
    d1l_settings_wifi_secret_wipe(&wifi_secret);

    memset(stored, 0, sizeof(stored));
    const size_t cleared_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, stored, sizeof(stored));
    payload = NULL;
    assert(d1l_settings_envelope_validate(
               stored, cleared_length, sizeof(d1l_settings_t),
               NULL, &payload) == D1L_SETTINGS_ENVELOPE_VALID);
    memset(&persisted, 0, sizeof(persisted));
    memcpy(&persisted, payload, sizeof(persisted));
    assert(!persisted.wifi_profile_saved);
    assert(bytes_all_zero(persisted.wifi_ssid,
                          sizeof(persisted.wifi_ssid)));
    assert(bytes_all_zero(persisted.wifi_password,
                          sizeof(persisted.wifi_password)));

    memset(seed, 0, sizeof(seed));
    memset(private_key, 0, sizeof(private_key));
    memset(&persisted, 0, sizeof(persisted));
}

typedef struct {
    d1l_settings_t settings;
    d1l_settings_update_mask_t update_mask;
    bool snapshot_only;
    esp_err_t result;
} overlapping_writer_t;

static atomic_int s_overlap_stage;

static void overlap_thread_yield(void)
{
#ifdef _WIN32
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

static void hold_first_writer_inside_nvs_set(void)
{
    atomic_store_explicit(&s_overlap_stage, 1, memory_order_release);
    while (atomic_load_explicit(&s_overlap_stage, memory_order_acquire) < 3) {
        overlap_thread_yield();
    }
}

static void mark_competing_lock_attempt(void)
{
    atomic_store_explicit(&s_overlap_stage, 2, memory_order_release);
}

static void execute_overlapping_writer(overlapping_writer_t *writer)
{
    if (writer->snapshot_only) {
        writer->result = d1l_settings_public_snapshot(&writer->settings);
    } else {
        writer->result = d1l_settings_update_fields(
            &writer->settings, writer->update_mask);
    }
}

#ifdef _WIN32
typedef HANDLE overlap_thread_t;

static DWORD WINAPI run_overlapping_writer(LPVOID context)
{
    execute_overlapping_writer((overlapping_writer_t *)context);
    return 0;
}

static bool start_overlap_thread(overlap_thread_t *thread,
                                 overlapping_writer_t *writer)
{
    *thread = CreateThread(NULL, 0U, run_overlapping_writer, writer, 0U, NULL);
    return *thread != NULL;
}

static bool join_overlap_thread(overlap_thread_t thread)
{
    const DWORD result = WaitForSingleObject(thread, INFINITE);
    return CloseHandle(thread) != 0 && result == WAIT_OBJECT_0;
}
#else
typedef pthread_t overlap_thread_t;

static void *run_overlapping_writer(void *context)
{
    execute_overlapping_writer((overlapping_writer_t *)context);
    return NULL;
}

static bool start_overlap_thread(overlap_thread_t *thread,
                                 overlapping_writer_t *writer)
{
    return pthread_create(thread, NULL, run_overlapping_writer, writer) == 0;
}

static bool join_overlap_thread(overlap_thread_t thread)
{
    return pthread_join(thread, NULL) == 0;
}
#endif

static void test_overlapping_writers_are_serialized(void)
{
    mock_nvs_reset();
    assert(d1l_settings_load() == ESP_OK);
    assert(d1l_settings_persistence_revision() == 1U);

    d1l_settings_t initial = {0};
    assert(d1l_settings_public_snapshot(&initial) == ESP_OK);

    overlapping_writer_t writer_a = {
        .settings = initial,
        .update_mask = D1L_SETTINGS_UPDATE_NODE_NAME,
        .snapshot_only = false,
        .result = ESP_ERR_INVALID_STATE,
    };
    overlapping_writer_t writer_b = {
        .settings = initial,
        .update_mask = D1L_SETTINGS_UPDATE_NIGHT_MODE,
        .snapshot_only = false,
        .result = ESP_ERR_INVALID_STATE,
    };
    (void)snprintf(writer_a.settings.node_name,
                   sizeof(writer_a.settings.node_name), "Writer A");
    writer_b.settings.night_mode = true;

    atomic_store_explicit(&s_overlap_stage, 0, memory_order_release);
    mock_nvs_run_during_next_set(hold_first_writer_inside_nvs_set);
    overlap_thread_t thread_a;
    overlap_thread_t thread_b;
    assert(start_overlap_thread(&thread_a, &writer_a));
    while (atomic_load_explicit(&s_overlap_stage, memory_order_acquire) < 1) {
        overlap_thread_yield();
    }
    mock_semaphore_run_after_takes(1U, mark_competing_lock_attempt);
    assert(start_overlap_thread(&thread_b, &writer_b));
    while (atomic_load_explicit(&s_overlap_stage, memory_order_acquire) < 2) {
        overlap_thread_yield();
    }
    atomic_store_explicit(&s_overlap_stage, 3, memory_order_release);
    assert(join_overlap_thread(thread_a));
    assert(join_overlap_thread(thread_b));
    assert(writer_a.result == ESP_OK);
    assert(writer_b.result == ESP_OK);
    assert(d1l_settings_persistence_revision() == 3U);

    uint8_t stored[TEST_BLOB_MAX] = {0};
    const size_t stored_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, stored, sizeof(stored));
    d1l_settings_envelope_header_t header = {0};
    const uint8_t *payload = NULL;
    assert(d1l_settings_envelope_validate(
               stored, stored_length, sizeof(d1l_settings_t),
           &header, &payload) == D1L_SETTINGS_ENVELOPE_VALID);
    assert(header.revision == 3U);
    d1l_settings_t current = {0};
    assert(d1l_settings_public_snapshot(&current) == ESP_OK);
    assert(strcmp(current.node_name, "Writer A") == 0);
    assert(current.night_mode);
    assert(memcmp(payload, &current, sizeof(current)) == 0);
}

static void test_snapshot_waits_for_coherent_publication(void)
{
    mock_nvs_reset();
    assert(d1l_settings_load() == ESP_OK);
    d1l_settings_t initial = {0};
    assert(d1l_settings_public_snapshot(&initial) == ESP_OK);

    overlapping_writer_t writer = {
        .settings = initial,
        .update_mask = D1L_SETTINGS_UPDATE_NODE_NAME,
        .snapshot_only = false,
        .result = ESP_ERR_INVALID_STATE,
    };
    overlapping_writer_t reader = {
        .settings = {0},
        .update_mask = 0U,
        .snapshot_only = true,
        .result = ESP_ERR_INVALID_STATE,
    };
    (void)snprintf(writer.settings.node_name,
                   sizeof(writer.settings.node_name), "Coherent snapshot");

    atomic_store_explicit(&s_overlap_stage, 0, memory_order_release);
    mock_nvs_run_during_next_set(hold_first_writer_inside_nvs_set);
    overlap_thread_t writer_thread;
    overlap_thread_t reader_thread;
    assert(start_overlap_thread(&writer_thread, &writer));
    while (atomic_load_explicit(&s_overlap_stage, memory_order_acquire) < 1) {
        overlap_thread_yield();
    }
    mock_semaphore_run_after_takes(1U, mark_competing_lock_attempt);
    assert(start_overlap_thread(&reader_thread, &reader));
    while (atomic_load_explicit(&s_overlap_stage, memory_order_acquire) < 2) {
        overlap_thread_yield();
    }
    atomic_store_explicit(&s_overlap_stage, 3, memory_order_release);
    assert(join_overlap_thread(writer_thread));
    assert(join_overlap_thread(reader_thread));
    assert(writer.result == ESP_OK);
    assert(reader.result == ESP_OK);
    assert(d1l_settings_persistence_revision() == 2U);
    assert(strcmp(reader.settings.node_name, "Coherent snapshot") == 0);

    uint8_t stored[TEST_BLOB_MAX] = {0};
    const size_t stored_length = mock_nvs_copy_blob(
        SETTINGS_NAMESPACE, SETTINGS_KEY, stored, sizeof(stored));
    const uint8_t *payload = NULL;
    assert(d1l_settings_envelope_validate(
               stored, stored_length, sizeof(d1l_settings_t),
               NULL, &payload) == D1L_SETTINGS_ENVELOPE_VALID);
    assert(memcmp(payload, &reader.settings, sizeof(reader.settings)) == 0);
}

int main(void)
{
    test_all_recognized_raw_legacy_versions_migrate();
    test_v7_envelope_migrates_revision_and_recovers_text();
    test_equal_size_v8_envelope_migrates_and_preserves_identity();
    test_invalid_timezone_recovers_to_utc_without_boot_loop();
    test_failed_legacy_rewrite_leaves_raw_blob_intact();
    test_unknown_newer_envelope_is_preserved_and_write_blocked();
    test_corruption_and_malformed_length_are_preserved();
    test_monotonic_save_and_revision_saturation();
    test_secret_update_bits_are_rejected();
    test_wifi_secret_tail_canonicalization();
    test_public_snapshot_redacts_typed_secrets();
    test_overlapping_writers_are_serialized();
    test_snapshot_waits_for_coherent_publication();
    puts("native settings persistence: ok");
    return 0;
}
