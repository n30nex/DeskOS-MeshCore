#include "settings_model.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ed_25519.h"
#include "nvs.h"

#include "d1l_config.h"
#include "mesh/store_lock.h"
#include "settings_envelope.h"

#define D1L_SETTINGS_NAMESPACE "d1l_settings"
#define D1L_SETTINGS_KEY "settings"
#define D1L_SETTINGS_MAX_PERSISTED_BLOB_SIZE 4096U

static d1l_settings_t s_current;
static bool s_loaded;
static esp_err_t s_load_status = ESP_ERR_INVALID_STATE;
static esp_err_t s_save_status = ESP_ERR_INVALID_STATE;
static uint32_t s_persistence_revision;
static bool s_persistence_write_blocked;
static d1l_settings_persistence_state_t s_persistence_state =
    D1L_SETTINGS_PERSISTENCE_UNINITIALIZED;
static d1l_store_lock_t s_settings_owner_lock = D1L_STORE_LOCK_INITIALIZER;

_Static_assert(D1L_IDENTITY_PUBLIC_KEY_LEN == D1L_IDENTITY_STATE_PUBLIC_KEY_LEN,
               "identity public key lengths must match");
_Static_assert(D1L_IDENTITY_PRIVATE_KEY_LEN == D1L_IDENTITY_STATE_PRIVATE_KEY_LEN,
               "identity private key lengths must match");
_Static_assert(D1L_WIFI_PROFILE_CAPACITY > 1U,
               "multi-profile Wi-Fi storage must retain more than one network");

static bool identity_bytes_all_zero(const uint8_t *bytes, size_t length)
{
    if (!bytes) {
        return false;
    }
    uint8_t combined = 0U;
    for (size_t i = 0; i < length; ++i) {
        combined |= bytes[i];
    }
    return combined == 0U;
}

static bool identity_bytes_uniform(const uint8_t *bytes, size_t length)
{
    if (!bytes || length == 0U) {
        return false;
    }
    uint8_t differences = 0U;
    for (size_t i = 1U; i < length; ++i) {
        differences |= (uint8_t)(bytes[i] ^ bytes[0]);
    }
    return differences == 0U;
}

static void wipe_sensitive_bytes(void *bytes, size_t length)
{
    volatile uint8_t *cursor = bytes;
    if (!cursor) {
        return;
    }
    for (size_t i = 0; i < length; ++i) {
        cursor[i] = 0U;
    }
}

static void copy_public_settings(d1l_settings_t *destination,
                                 const d1l_settings_t *source)
{
    if (!destination || !source) {
        return;
    }
    *destination = *source;
    wipe_sensitive_bytes(destination->wifi_password,
                         sizeof(destination->wifi_password));
    for (size_t i = 0U; i < D1L_WIFI_PROFILE_CAPACITY; ++i) {
        wipe_sensitive_bytes(
            destination->wifi_profiles[i].password,
            sizeof(destination->wifi_profiles[i].password));
    }
    wipe_sensitive_bytes(destination->identity_private_key,
                         sizeof(destination->identity_private_key));
}

static void wipe_derived_identity_key(
    uint8_t key[D1L_IDENTITY_STATE_PUBLIC_KEY_LEN])
{
    wipe_sensitive_bytes(key, D1L_IDENTITY_STATE_PUBLIC_KEY_LEN);
}

d1l_identity_state_t d1l_identity_state_classify(
    bool identity_ready,
    const uint8_t public_key[D1L_IDENTITY_STATE_PUBLIC_KEY_LEN],
    const uint8_t private_key[D1L_IDENTITY_STATE_PRIVATE_KEY_LEN])
{
    if (!public_key || !private_key) {
        return D1L_IDENTITY_STATE_INCONSISTENT;
    }

    const bool public_absent = identity_bytes_all_zero(
        public_key, D1L_IDENTITY_STATE_PUBLIC_KEY_LEN);
    const bool private_absent = identity_bytes_all_zero(
        private_key, D1L_IDENTITY_STATE_PRIVATE_KEY_LEN);
    if (!identity_ready) {
        return public_absent && private_absent
                   ? D1L_IDENTITY_STATE_ABSENT
                   : D1L_IDENTITY_STATE_INCONSISTENT;
    }
    if (public_absent || private_absent) {
        return D1L_IDENTITY_STATE_INCONSISTENT;
    }
    if ((private_key[0] & 0x07U) != 0U ||
        (private_key[31] & 0xc0U) != 0x40U) {
        /* ECDH reclamps the scalar; reject state whose stored scalar would
         * represent a different key after that operation. */
        return D1L_IDENTITY_STATE_INCONSISTENT;
    }
    if (identity_bytes_uniform(
            &private_key[32], D1L_IDENTITY_STATE_PRIVATE_KEY_LEN - 32U)) {
        /* Signing derives its nonce from this prefix. Uniform persisted data
         * is fail-closed because a predictable nonce can expose the scalar. */
        return D1L_IDENTITY_STATE_INCONSISTENT;
    }

    uint8_t derived_public[D1L_IDENTITY_STATE_PUBLIC_KEY_LEN] = {0};
    ed25519_derive_pub(derived_public, private_key);
    const bool exact_match = memcmp(
        public_key, derived_public, sizeof(derived_public)) == 0;
    wipe_derived_identity_key(derived_public);
    return exact_match ? D1L_IDENTITY_STATE_CONSISTENT
                       : D1L_IDENTITY_STATE_INCONSISTENT;
}

const char *d1l_identity_state_name(d1l_identity_state_t state)
{
    switch (state) {
        case D1L_IDENTITY_STATE_ABSENT:
            return "absent";
        case D1L_IDENTITY_STATE_CONSISTENT:
            return "consistent";
        case D1L_IDENTITY_STATE_INCONSISTENT:
        default:
            return "inconsistent";
    }
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
    uint8_t map_location_source;
    uint8_t map_tile_zoom;
    uint16_t timezone_schema_version;
    int16_t timezone_offset_minutes;
    bool identity_ready;
    uint8_t identity_public_key[D1L_IDENTITY_PUBLIC_KEY_LEN];
    uint8_t identity_private_key[D1L_IDENTITY_PRIVATE_KEY_LEN];
} d1l_settings_v9_t;

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
} d1l_settings_v8_t;

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
} d1l_settings_v6_t;

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
} d1l_settings_v7_t;

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
} d1l_settings_v5_t;

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
} d1l_settings_v4_t;

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
} d1l_settings_v3_t;

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
} d1l_settings_v2_t;

static uint16_t bandwidth_to_tenths(float khz)
{
    return (uint16_t)((khz * 10.0f) + 0.5f);
}

static void sanitize_printable(char *text, size_t size, bool allow_quotes)
{
    if (!text || size == 0) {
        return;
    }
    text[size - 1U] = '\0';
    for (size_t i = 0; i < size && text[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (c < 32 || c > 126 || (!allow_quotes && (c == '"' || c == '\\'))) {
            text[i] = '_';
        }
    }
}

static void zero_text_tail(char *text, size_t size, bool sensitive)
{
    if (!text || size == 0U) {
        return;
    }
    size_t length = 0U;
    while (length < size && text[length] != '\0') {
        length++;
    }
    if (length + 1U >= size) {
        return;
    }
    void *tail = &text[length + 1U];
    const size_t tail_length = size - length - 1U;
    if (sensitive) {
        wipe_sensitive_bytes(tail, tail_length);
    } else {
        memset(tail, 0, tail_length);
    }
}

static void reconcile_wifi_profiles(d1l_settings_t *settings)
{
    if (!settings) {
        return;
    }

    settings->wifi_ssid[D1L_WIFI_SSID_LEN - 1U] = '\0';
    settings->wifi_password[D1L_WIFI_PASSWORD_LEN - 1U] = '\0';
    sanitize_printable(
        settings->wifi_ssid, sizeof(settings->wifi_ssid), false);
    sanitize_printable(
        settings->wifi_password, sizeof(settings->wifi_password), true);
    settings->wifi_profile_saved =
        settings->wifi_profile_saved ? true : false;
    if (!settings->wifi_profile_saved ||
        settings->wifi_ssid[0] == '\0') {
        settings->wifi_profile_saved = false;
        memset(settings->wifi_ssid, 0, sizeof(settings->wifi_ssid));
        wipe_sensitive_bytes(
            settings->wifi_password, sizeof(settings->wifi_password));
    } else {
        zero_text_tail(
            settings->wifi_ssid, sizeof(settings->wifi_ssid), false);
        zero_text_tail(
            settings->wifi_password,
            sizeof(settings->wifi_password), true);
    }

    for (size_t i = 0U; i < D1L_WIFI_PROFILE_CAPACITY; ++i) {
        settings->wifi_profiles[i].ssid[
            D1L_WIFI_SSID_LEN - 1U] = '\0';
        settings->wifi_profiles[i].password[
            D1L_WIFI_PASSWORD_LEN - 1U] = '\0';
    }
    char preferred_ssid[D1L_WIFI_SSID_LEN] = {0};
    if (settings->wifi_active_profile < D1L_WIFI_PROFILE_CAPACITY &&
        settings->wifi_profiles[
            settings->wifi_active_profile].saved) {
        snprintf(
            preferred_ssid, sizeof(preferred_ssid), "%s",
            settings->wifi_profiles[
                settings->wifi_active_profile].ssid);
    } else if (settings->wifi_profile_saved) {
        snprintf(
            preferred_ssid, sizeof(preferred_ssid), "%s",
            settings->wifi_ssid);
    }

    d1l_wifi_profile_t compact[D1L_WIFI_PROFILE_CAPACITY] = {0};
    size_t compact_count = 0U;
    for (size_t i = 0U; i < D1L_WIFI_PROFILE_CAPACITY; ++i) {
        d1l_wifi_profile_t *profile = &settings->wifi_profiles[i];
        profile->saved = profile->saved ? true : false;
        profile->ssid[D1L_WIFI_SSID_LEN - 1U] = '\0';
        profile->password[D1L_WIFI_PASSWORD_LEN - 1U] = '\0';
        sanitize_printable(profile->ssid, sizeof(profile->ssid), false);
        sanitize_printable(
            profile->password, sizeof(profile->password), true);
        if (!profile->saved || profile->ssid[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (size_t existing = 0U;
             existing < compact_count; ++existing) {
            if (strcmp(compact[existing].ssid, profile->ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate || compact_count >= D1L_WIFI_PROFILE_CAPACITY) {
            continue;
        }
        compact[compact_count] = *profile;
        compact[compact_count].saved = true;
        zero_text_tail(
            compact[compact_count].ssid,
            sizeof(compact[compact_count].ssid), false);
        zero_text_tail(
            compact[compact_count].password,
            sizeof(compact[compact_count].password), true);
        compact_count++;
    }
    if (compact_count == 0U && settings->wifi_profile_saved) {
        compact[0].saved = true;
        snprintf(
            compact[0].ssid, sizeof(compact[0].ssid), "%s",
            settings->wifi_ssid);
        snprintf(
            compact[0].password, sizeof(compact[0].password), "%s",
            settings->wifi_password);
        compact_count = 1U;
    }

    size_t active = 0U;
    if (preferred_ssid[0] != '\0') {
        for (size_t i = 0U; i < compact_count; ++i) {
            if (strcmp(compact[i].ssid, preferred_ssid) == 0) {
                active = i;
                break;
            }
        }
    }
    wipe_sensitive_bytes(
        settings->wifi_profiles, sizeof(settings->wifi_profiles));
    if (compact_count > 0U) {
        memcpy(
            settings->wifi_profiles, compact,
            compact_count * sizeof(compact[0]));
        settings->wifi_profile_count = (uint8_t)compact_count;
        settings->wifi_active_profile = (uint8_t)active;
        settings->wifi_profile_saved = true;
        memset(settings->wifi_ssid, 0, sizeof(settings->wifi_ssid));
        snprintf(
            settings->wifi_ssid, sizeof(settings->wifi_ssid), "%s",
            compact[active].ssid);
        wipe_sensitive_bytes(
            settings->wifi_password, sizeof(settings->wifi_password));
        snprintf(
            settings->wifi_password, sizeof(settings->wifi_password), "%s",
            compact[active].password);
        zero_text_tail(
            settings->wifi_ssid, sizeof(settings->wifi_ssid), false);
        zero_text_tail(
            settings->wifi_password,
            sizeof(settings->wifi_password), true);
    } else {
        settings->wifi_profile_count = 0U;
        settings->wifi_active_profile = 0U;
        settings->wifi_profile_saved = false;
        memset(settings->wifi_ssid, 0, sizeof(settings->wifi_ssid));
        wipe_sensitive_bytes(
            settings->wifi_password, sizeof(settings->wifi_password));
    }
    wipe_sensitive_bytes(compact, sizeof(compact));
}

void d1l_settings_defaults(d1l_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }
    memset(settings, 0, sizeof(*settings));
    settings->schema_version = D1L_SETTINGS_SCHEMA_VERSION;
    snprintf(settings->node_name, sizeof(settings->node_name), "D1L Desk");
    settings->role = D1L_ROLE_DESK_COMPANION;
    settings->wifi_enabled = false;
    settings->ble_companion_enabled = false;
    settings->observer_enabled = false;
    settings->high_contrast = false;
    settings->night_mode = false;
    settings->onboarding_complete = false;
    settings->wifi_profile_saved = false;
    settings->path_hash_bytes = 1;
    settings->wifi_ssid[0] = '\0';
    settings->wifi_password[0] = '\0';
    settings->wifi_profile_count = 0U;
    settings->wifi_active_profile = 0U;
    settings->frequency_hz = D1L_RADIO_FREQ_HZ;
    settings->bandwidth_tenths_khz = bandwidth_to_tenths(D1L_RADIO_BW_KHZ);
    settings->spreading_factor = D1L_RADIO_SF;
    settings->coding_rate = D1L_RADIO_CR;
    settings->tx_power_dbm = D1L_RADIO_TX_POWER_DBM;
    settings->rx_boost = true;
    settings->tcxo_mode = D1L_TCXO_NONE;
    settings->map_location_set = false;
    settings->map_lat_e7 = 0;
    settings->map_lon_e7 = 0;
    settings->map_location_source = D1L_MAP_LOCATION_SOURCE_UNKNOWN;
    settings->map_tile_zoom = D1L_MAP_TILE_DEFAULT_ZOOM;
    settings->timezone_schema_version =
        D1L_TIMEZONE_SETTING_SCHEMA_VERSION;
    settings->timezone_offset_minutes = 0;
    settings->identity_ready = false;
    memset(settings->identity_public_key, 0, sizeof(settings->identity_public_key));
    memset(settings->identity_private_key, 0, sizeof(settings->identity_private_key));
}

void d1l_settings_sanitize(d1l_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }
    if (settings->schema_version != D1L_SETTINGS_SCHEMA_VERSION) {
        d1l_settings_defaults(settings);
        return;
    }
    settings->node_name[D1L_NODE_NAME_LEN - 1U] = '\0';
    if (settings->node_name[0] == '\0') {
        snprintf(settings->node_name, sizeof(settings->node_name), "D1L Desk");
    }
    sanitize_printable(settings->node_name, sizeof(settings->node_name), false);
    reconcile_wifi_profiles(settings);
    if (settings->role != D1L_ROLE_DESK_COMPANION) {
        settings->role = D1L_ROLE_DESK_COMPANION;
    }
    settings->onboarding_complete = settings->onboarding_complete ? true : false;
    settings->wifi_enabled = settings->wifi_enabled ? true : false;
    settings->ble_companion_enabled = settings->ble_companion_enabled ? true : false;
    if (settings->wifi_enabled && settings->ble_companion_enabled) {
        settings->ble_companion_enabled = false;
    }
    settings->observer_enabled = settings->observer_enabled ? true : false;
    if (settings->path_hash_bytes < 1 || settings->path_hash_bytes > 3) {
        settings->path_hash_bytes = 1;
    }
    if (settings->frequency_hz < 902000000UL || settings->frequency_hz > 928000000UL) {
        settings->frequency_hz = D1L_RADIO_FREQ_HZ;
    }
    if (settings->bandwidth_tenths_khz < 78 || settings->bandwidth_tenths_khz > 5000) {
        settings->bandwidth_tenths_khz = bandwidth_to_tenths(D1L_RADIO_BW_KHZ);
    }
    if (settings->spreading_factor < 5 || settings->spreading_factor > 12) {
        settings->spreading_factor = D1L_RADIO_SF;
    }
    if (settings->coding_rate < 5 || settings->coding_rate > 8) {
        settings->coding_rate = D1L_RADIO_CR;
    }
    if (settings->tx_power_dbm > D1L_RADIO_TX_POWER_DBM) {
        settings->tx_power_dbm = D1L_RADIO_TX_POWER_DBM;
    }
    if (settings->tx_power_dbm < -9) {
        settings->tx_power_dbm = -9;
    }
    settings->tcxo_mode = D1L_TCXO_NONE;
    settings->map_location_set = settings->map_location_set ? true : false;
    if (!settings->map_location_set ||
        settings->map_lat_e7 < D1L_MAP_LOCATION_LAT_E7_MIN ||
        settings->map_lat_e7 > D1L_MAP_LOCATION_LAT_E7_MAX ||
        settings->map_lon_e7 < D1L_MAP_LOCATION_LON_E7_MIN ||
        settings->map_lon_e7 > D1L_MAP_LOCATION_LON_E7_MAX) {
        settings->map_location_set = false;
        settings->map_lat_e7 = 0;
        settings->map_lon_e7 = 0;
        settings->map_location_source = D1L_MAP_LOCATION_SOURCE_UNKNOWN;
    } else if (settings->map_location_source !=
                   D1L_MAP_LOCATION_SOURCE_MANUAL &&
               settings->map_location_source !=
                   D1L_MAP_LOCATION_SOURCE_AUTHENTICATED_COMPANION) {
        settings->map_location_source = D1L_MAP_LOCATION_SOURCE_MANUAL;
    }
    settings->map_tile_zoom = D1L_MAP_TILE_DEFAULT_ZOOM;
    if (settings->timezone_schema_version !=
            D1L_TIMEZONE_SETTING_SCHEMA_VERSION ||
        !d1l_time_display_offset_valid(
            settings->timezone_offset_minutes)) {
        settings->timezone_schema_version =
            D1L_TIMEZONE_SETTING_SCHEMA_VERSION;
        settings->timezone_offset_minutes = 0;
    }
    settings->identity_ready = settings->identity_ready ? true : false;
    if (d1l_settings_identity_state(settings) != D1L_IDENTITY_STATE_CONSISTENT) {
        /* Fail closed without destroying forensic/recovery evidence. */
        settings->identity_ready = false;
    }
}

d1l_identity_state_t d1l_settings_identity_state(const d1l_settings_t *settings)
{
    if (!settings) {
        return D1L_IDENTITY_STATE_INCONSISTENT;
    }
    return d1l_identity_state_classify(
        settings->identity_ready,
        settings->identity_public_key,
        settings->identity_private_key);
}

static void migrate_v2_settings(d1l_settings_t *dest, const d1l_settings_v2_t *src)
{
    if (!dest) {
        return;
    }
    d1l_settings_defaults(dest);
    if (!src || src->schema_version != 2U) {
        return;
    }
    memcpy(dest->node_name, src->node_name, sizeof(dest->node_name));
    dest->node_name[D1L_NODE_NAME_LEN - 1U] = '\0';
    dest->role = src->role;
    dest->wifi_enabled = src->wifi_enabled;
    dest->ble_companion_enabled = src->ble_companion_enabled;
    dest->observer_enabled = src->observer_enabled;
    dest->high_contrast = src->high_contrast;
    dest->night_mode = src->night_mode;
    dest->onboarding_complete = true;
    dest->path_hash_bytes = src->path_hash_bytes;
    dest->frequency_hz = src->frequency_hz;
    dest->bandwidth_tenths_khz = src->bandwidth_tenths_khz;
    dest->spreading_factor = src->spreading_factor;
    dest->coding_rate = src->coding_rate;
    dest->tx_power_dbm = src->tx_power_dbm;
    dest->rx_boost = src->rx_boost;
    dest->tcxo_mode = src->tcxo_mode;
    dest->identity_ready = src->identity_ready;
    memcpy(dest->identity_public_key, src->identity_public_key, sizeof(dest->identity_public_key));
    memcpy(dest->identity_private_key, src->identity_private_key, sizeof(dest->identity_private_key));
    dest->schema_version = D1L_SETTINGS_SCHEMA_VERSION;
    d1l_settings_sanitize(dest);
}

static void migrate_v3_settings(d1l_settings_t *dest, const d1l_settings_v3_t *src)
{
    if (!dest) {
        return;
    }
    d1l_settings_defaults(dest);
    if (!src || src->schema_version != 3U) {
        return;
    }
    memcpy(dest->node_name, src->node_name, sizeof(dest->node_name));
    dest->node_name[D1L_NODE_NAME_LEN - 1U] = '\0';
    dest->role = src->role;
    dest->wifi_enabled = src->wifi_enabled;
    dest->ble_companion_enabled = src->ble_companion_enabled;
    dest->observer_enabled = src->observer_enabled;
    dest->high_contrast = src->high_contrast;
    dest->night_mode = src->night_mode;
    dest->onboarding_complete = src->onboarding_complete;
    dest->path_hash_bytes = src->path_hash_bytes;
    dest->frequency_hz = src->frequency_hz;
    dest->bandwidth_tenths_khz = src->bandwidth_tenths_khz;
    dest->spreading_factor = src->spreading_factor;
    dest->coding_rate = src->coding_rate;
    dest->tx_power_dbm = src->tx_power_dbm;
    dest->rx_boost = src->rx_boost;
    dest->tcxo_mode = src->tcxo_mode;
    dest->map_location_set = false;
    dest->map_lat_e7 = 0;
    dest->map_lon_e7 = 0;
    dest->identity_ready = src->identity_ready;
    memcpy(dest->identity_public_key, src->identity_public_key, sizeof(dest->identity_public_key));
    memcpy(dest->identity_private_key, src->identity_private_key, sizeof(dest->identity_private_key));
    dest->schema_version = D1L_SETTINGS_SCHEMA_VERSION;
    d1l_settings_sanitize(dest);
}

static void migrate_v5_settings(d1l_settings_t *dest, const d1l_settings_v5_t *src)
{
    if (!dest) {
        return;
    }
    d1l_settings_defaults(dest);
    if (!src || src->schema_version != 5U) {
        return;
    }
    memcpy(dest->node_name, src->node_name, sizeof(dest->node_name));
    dest->node_name[D1L_NODE_NAME_LEN - 1U] = '\0';
    dest->role = src->role;
    dest->wifi_enabled = src->wifi_enabled;
    dest->ble_companion_enabled = src->ble_companion_enabled;
    dest->observer_enabled = src->observer_enabled;
    dest->high_contrast = src->high_contrast;
    dest->night_mode = src->night_mode;
    dest->onboarding_complete = src->onboarding_complete;
    dest->wifi_profile_saved = src->wifi_profile_saved;
    memcpy(dest->wifi_ssid, src->wifi_ssid, sizeof(dest->wifi_ssid));
    dest->wifi_ssid[D1L_WIFI_SSID_LEN - 1U] = '\0';
    memcpy(dest->wifi_password, src->wifi_password, sizeof(dest->wifi_password));
    dest->wifi_password[D1L_WIFI_PASSWORD_LEN - 1U] = '\0';
    dest->path_hash_bytes = src->path_hash_bytes;
    dest->frequency_hz = src->frequency_hz;
    dest->bandwidth_tenths_khz = src->bandwidth_tenths_khz;
    dest->spreading_factor = src->spreading_factor;
    dest->coding_rate = src->coding_rate;
    dest->tx_power_dbm = src->tx_power_dbm;
    dest->rx_boost = src->rx_boost;
    dest->tcxo_mode = src->tcxo_mode;
    dest->map_location_set = src->map_location_set;
    dest->map_lat_e7 = src->map_lat_e7;
    dest->map_lon_e7 = src->map_lon_e7;
    dest->map_tile_zoom = D1L_MAP_TILE_DEFAULT_ZOOM;
    dest->identity_ready = src->identity_ready;
    memcpy(dest->identity_public_key, src->identity_public_key, sizeof(dest->identity_public_key));
    memcpy(dest->identity_private_key, src->identity_private_key, sizeof(dest->identity_private_key));
    dest->schema_version = D1L_SETTINGS_SCHEMA_VERSION;
    d1l_settings_sanitize(dest);
}

static void migrate_v7_settings(d1l_settings_t *dest, const d1l_settings_v7_t *src)
{
    if (!dest) {
        return;
    }
    d1l_settings_defaults(dest);
    if (!src || src->schema_version != 7U) {
        return;
    }
    memcpy(dest->node_name, src->node_name, sizeof(dest->node_name));
    dest->node_name[D1L_NODE_NAME_LEN - 1U] = '\0';
    dest->role = src->role;
    dest->wifi_enabled = src->wifi_enabled;
    dest->ble_companion_enabled = src->ble_companion_enabled;
    dest->observer_enabled = src->observer_enabled;
    dest->high_contrast = src->high_contrast;
    dest->night_mode = src->night_mode;
    dest->onboarding_complete = src->onboarding_complete;
    dest->wifi_profile_saved = src->wifi_profile_saved;
    memcpy(dest->wifi_ssid, src->wifi_ssid, sizeof(dest->wifi_ssid));
    dest->wifi_ssid[D1L_WIFI_SSID_LEN - 1U] = '\0';
    memcpy(dest->wifi_password, src->wifi_password,
           sizeof(dest->wifi_password));
    dest->wifi_password[D1L_WIFI_PASSWORD_LEN - 1U] = '\0';
    dest->path_hash_bytes = src->path_hash_bytes;
    dest->frequency_hz = src->frequency_hz;
    dest->bandwidth_tenths_khz = src->bandwidth_tenths_khz;
    dest->spreading_factor = src->spreading_factor;
    dest->coding_rate = src->coding_rate;
    dest->tx_power_dbm = src->tx_power_dbm;
    dest->rx_boost = src->rx_boost;
    dest->tcxo_mode = src->tcxo_mode;
    dest->map_location_set = src->map_location_set;
    dest->map_lat_e7 = src->map_lat_e7;
    dest->map_lon_e7 = src->map_lon_e7;
    dest->map_tile_zoom = src->map_tile_zoom;
    dest->identity_ready = src->identity_ready;
    memcpy(dest->identity_public_key, src->identity_public_key,
           sizeof(dest->identity_public_key));
    memcpy(dest->identity_private_key, src->identity_private_key,
           sizeof(dest->identity_private_key));
    dest->schema_version = D1L_SETTINGS_SCHEMA_VERSION;
    d1l_settings_sanitize(dest);
}

static void migrate_v9_settings(d1l_settings_t *dest,
                                const d1l_settings_v9_t *src)
{
    if (!dest) {
        return;
    }
    d1l_settings_defaults(dest);
    if (!src || src->schema_version != 9U) {
        return;
    }
    memcpy(dest->node_name, src->node_name, sizeof(dest->node_name));
    dest->node_name[D1L_NODE_NAME_LEN - 1U] = '\0';
    dest->role = src->role;
    dest->wifi_enabled = src->wifi_enabled;
    dest->ble_companion_enabled = src->ble_companion_enabled;
    dest->observer_enabled = src->observer_enabled;
    dest->high_contrast = src->high_contrast;
    dest->night_mode = src->night_mode;
    dest->onboarding_complete = src->onboarding_complete;
    dest->wifi_profile_saved = src->wifi_profile_saved;
    memcpy(dest->wifi_ssid, src->wifi_ssid, sizeof(dest->wifi_ssid));
    dest->wifi_ssid[D1L_WIFI_SSID_LEN - 1U] = '\0';
    memcpy(
        dest->wifi_password, src->wifi_password,
        sizeof(dest->wifi_password));
    dest->wifi_password[D1L_WIFI_PASSWORD_LEN - 1U] = '\0';
    dest->path_hash_bytes = src->path_hash_bytes;
    dest->frequency_hz = src->frequency_hz;
    dest->bandwidth_tenths_khz = src->bandwidth_tenths_khz;
    dest->spreading_factor = src->spreading_factor;
    dest->coding_rate = src->coding_rate;
    dest->tx_power_dbm = src->tx_power_dbm;
    dest->rx_boost = src->rx_boost;
    dest->tcxo_mode = src->tcxo_mode;
    dest->map_location_set = src->map_location_set;
    dest->map_lat_e7 = src->map_lat_e7;
    dest->map_lon_e7 = src->map_lon_e7;
    dest->map_location_source = src->map_location_source;
    dest->map_tile_zoom = src->map_tile_zoom;
    dest->timezone_schema_version = src->timezone_schema_version;
    dest->timezone_offset_minutes = src->timezone_offset_minutes;
    dest->identity_ready = src->identity_ready;
    memcpy(
        dest->identity_public_key, src->identity_public_key,
        sizeof(dest->identity_public_key));
    memcpy(
        dest->identity_private_key, src->identity_private_key,
        sizeof(dest->identity_private_key));
    dest->schema_version = D1L_SETTINGS_SCHEMA_VERSION;
    d1l_settings_sanitize(dest);
}

static void migrate_v8_settings(d1l_settings_t *dest,
                                const d1l_settings_v8_t *src)
{
    if (!dest) {
        return;
    }
    d1l_settings_defaults(dest);
    if (!src || src->schema_version != 8U) {
        return;
    }
    memcpy(dest->node_name, src->node_name, sizeof(dest->node_name));
    dest->node_name[D1L_NODE_NAME_LEN - 1U] = '\0';
    dest->role = src->role;
    dest->wifi_enabled = src->wifi_enabled;
    dest->ble_companion_enabled = src->ble_companion_enabled;
    dest->observer_enabled = src->observer_enabled;
    dest->high_contrast = src->high_contrast;
    dest->night_mode = src->night_mode;
    dest->onboarding_complete = src->onboarding_complete;
    dest->wifi_profile_saved = src->wifi_profile_saved;
    memcpy(dest->wifi_ssid, src->wifi_ssid, sizeof(dest->wifi_ssid));
    dest->wifi_ssid[D1L_WIFI_SSID_LEN - 1U] = '\0';
    memcpy(dest->wifi_password, src->wifi_password,
           sizeof(dest->wifi_password));
    dest->wifi_password[D1L_WIFI_PASSWORD_LEN - 1U] = '\0';
    dest->path_hash_bytes = src->path_hash_bytes;
    dest->frequency_hz = src->frequency_hz;
    dest->bandwidth_tenths_khz = src->bandwidth_tenths_khz;
    dest->spreading_factor = src->spreading_factor;
    dest->coding_rate = src->coding_rate;
    dest->tx_power_dbm = src->tx_power_dbm;
    dest->rx_boost = src->rx_boost;
    dest->tcxo_mode = src->tcxo_mode;
    dest->map_location_set = src->map_location_set;
    dest->map_lat_e7 = src->map_lat_e7;
    dest->map_lon_e7 = src->map_lon_e7;
    dest->map_location_source = src->map_location_set ?
        D1L_MAP_LOCATION_SOURCE_MANUAL :
        D1L_MAP_LOCATION_SOURCE_UNKNOWN;
    dest->map_tile_zoom = src->map_tile_zoom;
    dest->timezone_schema_version = src->timezone_schema_version;
    dest->timezone_offset_minutes = src->timezone_offset_minutes;
    dest->identity_ready = src->identity_ready;
    memcpy(dest->identity_public_key, src->identity_public_key,
           sizeof(dest->identity_public_key));
    memcpy(dest->identity_private_key, src->identity_private_key,
           sizeof(dest->identity_private_key));
    dest->schema_version = D1L_SETTINGS_SCHEMA_VERSION;
    d1l_settings_sanitize(dest);
}

static void migrate_v6_settings(d1l_settings_t *dest, const d1l_settings_v6_t *src)
{
    if (!dest) {
        return;
    }
    d1l_settings_defaults(dest);
    if (!src || src->schema_version != 6U) {
        return;
    }
    memcpy(dest->node_name, src->node_name, sizeof(dest->node_name));
    dest->node_name[D1L_NODE_NAME_LEN - 1U] = '\0';
    dest->role = src->role;
    dest->wifi_enabled = src->wifi_enabled;
    dest->ble_companion_enabled = src->ble_companion_enabled;
    dest->observer_enabled = src->observer_enabled;
    dest->high_contrast = src->high_contrast;
    dest->night_mode = src->night_mode;
    dest->onboarding_complete = src->onboarding_complete;
    dest->wifi_profile_saved = src->wifi_profile_saved;
    memcpy(dest->wifi_ssid, src->wifi_ssid, sizeof(dest->wifi_ssid));
    dest->wifi_ssid[D1L_WIFI_SSID_LEN - 1U] = '\0';
    memcpy(dest->wifi_password, src->wifi_password, sizeof(dest->wifi_password));
    dest->wifi_password[D1L_WIFI_PASSWORD_LEN - 1U] = '\0';
    dest->path_hash_bytes = src->path_hash_bytes;
    dest->frequency_hz = src->frequency_hz;
    dest->bandwidth_tenths_khz = src->bandwidth_tenths_khz;
    dest->spreading_factor = src->spreading_factor;
    dest->coding_rate = src->coding_rate;
    dest->tx_power_dbm = src->tx_power_dbm;
    dest->rx_boost = src->rx_boost;
    dest->tcxo_mode = src->tcxo_mode;
    dest->map_location_set = src->map_location_set;
    dest->map_lat_e7 = src->map_lat_e7;
    dest->map_lon_e7 = src->map_lon_e7;
    dest->map_tile_zoom = D1L_MAP_TILE_DEFAULT_ZOOM;
    dest->identity_ready = src->identity_ready;
    memcpy(dest->identity_public_key, src->identity_public_key,
           sizeof(dest->identity_public_key));
    memcpy(dest->identity_private_key, src->identity_private_key,
           sizeof(dest->identity_private_key));
    dest->schema_version = D1L_SETTINGS_SCHEMA_VERSION;
    d1l_settings_sanitize(dest);
}

static void migrate_v4_settings(d1l_settings_t *dest, const d1l_settings_v4_t *src)
{
    if (!dest) {
        return;
    }
    d1l_settings_defaults(dest);
    if (!src || src->schema_version != 4U) {
        return;
    }
    memcpy(dest->node_name, src->node_name, sizeof(dest->node_name));
    dest->node_name[D1L_NODE_NAME_LEN - 1U] = '\0';
    dest->role = src->role;
    dest->wifi_enabled = src->wifi_enabled;
    dest->ble_companion_enabled = src->ble_companion_enabled;
    dest->observer_enabled = src->observer_enabled;
    dest->high_contrast = src->high_contrast;
    dest->night_mode = src->night_mode;
    dest->onboarding_complete = src->onboarding_complete;
    dest->path_hash_bytes = src->path_hash_bytes;
    dest->frequency_hz = src->frequency_hz;
    dest->bandwidth_tenths_khz = src->bandwidth_tenths_khz;
    dest->spreading_factor = src->spreading_factor;
    dest->coding_rate = src->coding_rate;
    dest->tx_power_dbm = src->tx_power_dbm;
    dest->rx_boost = src->rx_boost;
    dest->tcxo_mode = src->tcxo_mode;
    dest->map_location_set = src->map_location_set;
    dest->map_lat_e7 = src->map_lat_e7;
    dest->map_lon_e7 = src->map_lon_e7;
    dest->identity_ready = src->identity_ready;
    memcpy(dest->identity_public_key, src->identity_public_key, sizeof(dest->identity_public_key));
    memcpy(dest->identity_private_key, src->identity_private_key, sizeof(dest->identity_private_key));
    dest->schema_version = D1L_SETTINGS_SCHEMA_VERSION;
    d1l_settings_sanitize(dest);
}

static esp_err_t persist_settings_envelope(nvs_handle_t handle,
                                           const d1l_settings_t *settings,
                                           uint32_t revision)
{
    uint8_t blob[sizeof(d1l_settings_envelope_header_t) +
                 sizeof(d1l_settings_t)] = {0};
    size_t blob_length = 0U;
    if (!d1l_settings_envelope_build(blob, sizeof(blob), settings,
                                     sizeof(*settings), revision,
                                     &blob_length)) {
        wipe_sensitive_bytes(blob, sizeof(blob));
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = nvs_set_blob(handle, D1L_SETTINGS_KEY, blob, blob_length);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    wipe_sensitive_bytes(blob, sizeof(blob));
    return ret;
}

static bool migrate_legacy_settings_blob(d1l_settings_t *destination,
                                         const uint8_t *blob,
                                         size_t blob_length,
                                         bool *schema_newer)
{
    if (schema_newer) {
        *schema_newer = false;
    }
    if (!destination || !blob || blob_length < sizeof(uint32_t)) {
        return false;
    }

    uint32_t schema_version = 0U;
    memcpy(&schema_version, blob, sizeof(schema_version));
    if (schema_version > D1L_SETTINGS_SCHEMA_VERSION) {
        if (schema_newer) {
            *schema_newer = true;
        }
        return false;
    }

    switch (schema_version) {
    case D1L_SETTINGS_SCHEMA_VERSION:
        if (blob_length != sizeof(d1l_settings_t)) {
            return false;
        }
        memcpy(destination, blob, sizeof(*destination));
        d1l_settings_sanitize(destination);
        return true;
    case 9U:
        if (blob_length != sizeof(d1l_settings_v9_t)) {
            return false;
        }
        {
            d1l_settings_v9_t legacy = {0};
            memcpy(&legacy, blob, sizeof(legacy));
            migrate_v9_settings(destination, &legacy);
            wipe_sensitive_bytes(&legacy, sizeof(legacy));
        }
        return true;
    case 8U:
        if (blob_length != sizeof(d1l_settings_v8_t)) {
            return false;
        }
        {
            d1l_settings_v8_t legacy = {0};
            memcpy(&legacy, blob, sizeof(legacy));
            migrate_v8_settings(destination, &legacy);
            wipe_sensitive_bytes(&legacy, sizeof(legacy));
        }
        return true;
    case 7U:
        if (blob_length != sizeof(d1l_settings_v7_t)) {
            return false;
        }
        {
            d1l_settings_v7_t legacy = {0};
            memcpy(&legacy, blob, sizeof(legacy));
            migrate_v7_settings(destination, &legacy);
            wipe_sensitive_bytes(&legacy, sizeof(legacy));
        }
        return true;
    case 6U:
        if (blob_length != sizeof(d1l_settings_v6_t)) {
            return false;
        }
        {
            d1l_settings_v6_t legacy = {0};
            memcpy(&legacy, blob, sizeof(legacy));
            migrate_v6_settings(destination, &legacy);
            wipe_sensitive_bytes(&legacy, sizeof(legacy));
        }
        return true;
    case 5U:
        if (blob_length != sizeof(d1l_settings_v5_t)) {
            return false;
        }
        {
            d1l_settings_v5_t legacy = {0};
            memcpy(&legacy, blob, sizeof(legacy));
            migrate_v5_settings(destination, &legacy);
            wipe_sensitive_bytes(&legacy, sizeof(legacy));
        }
        return true;
    case 4U:
        if (blob_length != sizeof(d1l_settings_v4_t)) {
            return false;
        }
        {
            d1l_settings_v4_t legacy = {0};
            memcpy(&legacy, blob, sizeof(legacy));
            migrate_v4_settings(destination, &legacy);
            wipe_sensitive_bytes(&legacy, sizeof(legacy));
        }
        return true;
    case 3U:
        if (blob_length != sizeof(d1l_settings_v3_t)) {
            return false;
        }
        {
            d1l_settings_v3_t legacy = {0};
            memcpy(&legacy, blob, sizeof(legacy));
            migrate_v3_settings(destination, &legacy);
            wipe_sensitive_bytes(&legacy, sizeof(legacy));
        }
        return true;
    case 2U:
        if (blob_length != sizeof(d1l_settings_v2_t)) {
            return false;
        }
        {
            d1l_settings_v2_t legacy = {0};
            memcpy(&legacy, blob, sizeof(legacy));
            migrate_v2_settings(destination, &legacy);
            wipe_sensitive_bytes(&legacy, sizeof(legacy));
        }
        return true;
    default:
        return false;
    }
}

static esp_err_t quarantine_settings(
    d1l_settings_persistence_state_t state, esp_err_t error)
{
    s_loaded = false;
    s_persistence_revision = 0U;
    s_persistence_write_blocked = true;
    s_persistence_state = state;
    return error;
}

static bool settings_owner_take(void)
{
    SemaphoreHandle_t handle = d1l_store_lock_handle(&s_settings_owner_lock);
    return handle && xSemaphoreTake(handle, portMAX_DELAY) == pdTRUE;
}

static void settings_owner_give(void)
{
    d1l_store_lock_give(&s_settings_owner_lock);
}

static esp_err_t settings_load_locked(void)
{
    s_load_status = ESP_ERR_INVALID_STATE;
    s_save_status = ESP_ERR_INVALID_STATE;
    s_loaded = false;
    s_persistence_revision = 0U;
    s_persistence_write_blocked = false;
    s_persistence_state = D1L_SETTINGS_PERSISTENCE_UNINITIALIZED;
    d1l_settings_defaults(&s_current);

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        s_persistence_state = D1L_SETTINGS_PERSISTENCE_IO_ERROR;
        s_load_status = ret;
        return ret;
    }

    size_t len = 0U;
    ret = nvs_get_blob(handle, D1L_SETTINGS_KEY, NULL, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        d1l_settings_defaults(&s_current);
        ret = persist_settings_envelope(handle, &s_current, 1U);
        if (ret == ESP_OK) {
            s_persistence_revision = 1U;
            s_persistence_state =
                D1L_SETTINGS_PERSISTENCE_INITIALIZED_DEFAULTS;
        }
    } else if (ret == ESP_OK &&
               (len == 0U || len > D1L_SETTINGS_MAX_PERSISTED_BLOB_SIZE)) {
        ret = quarantine_settings(
            D1L_SETTINGS_PERSISTENCE_QUARANTINED_MALFORMED,
            ESP_ERR_INVALID_SIZE);
    } else if (ret == ESP_OK) {
        uint8_t *blob = malloc(len);
        if (!blob) {
            ret = ESP_ERR_NO_MEM;
        } else {
            size_t read_length = len;
            ret = nvs_get_blob(handle, D1L_SETTINGS_KEY, blob, &read_length);
            if (ret == ESP_OK && read_length != len) {
                ret = ESP_ERR_INVALID_SIZE;
            }
            if (ret == ESP_OK) {
                d1l_settings_envelope_header_t header = {0};
                const uint8_t *payload = NULL;
                const d1l_settings_envelope_validation_t validation =
                    d1l_settings_envelope_validate(
                        blob, len, sizeof(d1l_settings_t), &header, &payload);
                if (validation == D1L_SETTINGS_ENVELOPE_VALID) {
                    uint32_t payload_schema_version = 0U;
                    memcpy(&payload_schema_version, payload,
                           sizeof(payload_schema_version));
                    if (payload_schema_version ==
                        D1L_SETTINGS_SCHEMA_VERSION) {
                        d1l_settings_t loaded = {0};
                        memcpy(&loaded, payload, sizeof(loaded));
                        s_current = loaded;
                        d1l_settings_sanitize(&s_current);
                        s_persistence_revision = header.revision;
                        s_persistence_state =
                            D1L_SETTINGS_PERSISTENCE_READY;
                        wipe_sensitive_bytes(&loaded, sizeof(loaded));
                    } else {
                        /* Adjacent schemas can have the same ABI size. The
                         * envelope length is therefore insufficient to prove
                         * that its payload uses the current field layout. */
                        bool schema_newer = false;
                        d1l_settings_t migrated = {0};
                        if (migrate_legacy_settings_blob(
                                &migrated, payload, header.payload_length,
                                &schema_newer)) {
                            uint32_t next_revision = 0U;
                            if (!d1l_settings_envelope_next_revision(
                                    header.revision, &next_revision)) {
                                ret = quarantine_settings(
                                    D1L_SETTINGS_PERSISTENCE_REVISION_SATURATED,
                                    ESP_ERR_INVALID_STATE);
                            } else {
                                ret = persist_settings_envelope(
                                    handle, &migrated, next_revision);
                                if (ret == ESP_OK) {
                                    s_current = migrated;
                                    s_persistence_revision = next_revision;
                                    s_persistence_state =
                                        D1L_SETTINGS_PERSISTENCE_MIGRATED_LEGACY;
                                }
                            }
                        } else if (schema_newer) {
                            ret = quarantine_settings(
                                D1L_SETTINGS_PERSISTENCE_QUARANTINED_NEWER_SCHEMA,
                                ESP_ERR_NOT_SUPPORTED);
                        } else {
                            ret = quarantine_settings(
                                D1L_SETTINGS_PERSISTENCE_QUARANTINED_MALFORMED,
                                ESP_ERR_INVALID_SIZE);
                        }
                        wipe_sensitive_bytes(&migrated, sizeof(migrated));
                    }
                } else if (validation ==
                           D1L_SETTINGS_ENVELOPE_SCHEMA_NEWER) {
                    ret = quarantine_settings(
                        D1L_SETTINGS_PERSISTENCE_QUARANTINED_NEWER_SCHEMA,
                        ESP_ERR_NOT_SUPPORTED);
                } else if (validation ==
                           D1L_SETTINGS_ENVELOPE_CHECKSUM_MISMATCH) {
                    ret = quarantine_settings(
                        D1L_SETTINGS_PERSISTENCE_QUARANTINED_CHECKSUM,
                        ESP_FAIL);
                } else if (validation ==
                           D1L_SETTINGS_ENVELOPE_MALFORMED) {
                    /* A prior settings payload has a different size but a
                     * stable envelope. Validate that envelope against its
                     * declared length before interpreting the versioned
                     * payload; all other malformed state remains preserved. */
                    d1l_settings_envelope_header_t legacy_header = {0};
                    const uint8_t *legacy_payload = NULL;
                    const d1l_settings_envelope_validation_t
                        legacy_validation = d1l_settings_envelope_validate(
                            blob, len, header.payload_length,
                            &legacy_header, &legacy_payload);
                    if (legacy_validation == D1L_SETTINGS_ENVELOPE_VALID) {
                        bool schema_newer = false;
                        d1l_settings_t migrated = {0};
                        if (migrate_legacy_settings_blob(
                                &migrated, legacy_payload,
                                legacy_header.payload_length,
                                &schema_newer)) {
                            uint32_t next_revision = 0U;
                            if (!d1l_settings_envelope_next_revision(
                                    legacy_header.revision,
                                    &next_revision)) {
                                ret = quarantine_settings(
                                    D1L_SETTINGS_PERSISTENCE_REVISION_SATURATED,
                                    ESP_ERR_INVALID_STATE);
                            } else {
                                ret = persist_settings_envelope(
                                    handle, &migrated, next_revision);
                                if (ret == ESP_OK) {
                                    s_current = migrated;
                                    s_persistence_revision = next_revision;
                                    s_persistence_state =
                                        D1L_SETTINGS_PERSISTENCE_MIGRATED_LEGACY;
                                }
                            }
                        } else if (schema_newer) {
                            ret = quarantine_settings(
                                D1L_SETTINGS_PERSISTENCE_QUARANTINED_NEWER_SCHEMA,
                                ESP_ERR_NOT_SUPPORTED);
                        } else {
                            ret = quarantine_settings(
                                D1L_SETTINGS_PERSISTENCE_QUARANTINED_MALFORMED,
                                ESP_ERR_INVALID_SIZE);
                        }
                        wipe_sensitive_bytes(&migrated, sizeof(migrated));
                    } else if (legacy_validation ==
                               D1L_SETTINGS_ENVELOPE_CHECKSUM_MISMATCH) {
                        ret = quarantine_settings(
                            D1L_SETTINGS_PERSISTENCE_QUARANTINED_CHECKSUM,
                            ESP_FAIL);
                    } else if (legacy_validation ==
                               D1L_SETTINGS_ENVELOPE_SCHEMA_NEWER) {
                        ret = quarantine_settings(
                            D1L_SETTINGS_PERSISTENCE_QUARANTINED_NEWER_SCHEMA,
                            ESP_ERR_NOT_SUPPORTED);
                    } else {
                        ret = quarantine_settings(
                            D1L_SETTINGS_PERSISTENCE_QUARANTINED_MALFORMED,
                            ESP_ERR_INVALID_SIZE);
                    }
                } else {
                    bool schema_newer = false;
                    d1l_settings_t migrated = {0};
                    if (migrate_legacy_settings_blob(
                            &migrated, blob, len, &schema_newer)) {
                        ret = persist_settings_envelope(
                            handle, &migrated, 1U);
                        if (ret == ESP_OK) {
                            s_current = migrated;
                            s_persistence_revision = 1U;
                            s_persistence_state =
                                D1L_SETTINGS_PERSISTENCE_MIGRATED_LEGACY;
                        }
                    } else if (schema_newer) {
                        ret = quarantine_settings(
                            D1L_SETTINGS_PERSISTENCE_QUARANTINED_NEWER_SCHEMA,
                            ESP_ERR_NOT_SUPPORTED);
                    } else {
                        ret = quarantine_settings(
                            D1L_SETTINGS_PERSISTENCE_QUARANTINED_MALFORMED,
                            ESP_ERR_INVALID_SIZE);
                    }
                    wipe_sensitive_bytes(&migrated, sizeof(migrated));
                }
            }
            wipe_sensitive_bytes(blob, len);
            free(blob);
        }
    }
    nvs_close(handle);
    if (ret != ESP_OK && !s_persistence_write_blocked) {
        s_persistence_state = D1L_SETTINGS_PERSISTENCE_IO_ERROR;
    }
    s_loaded = (ret == ESP_OK);
    s_load_status = ret;
    return ret;
}

esp_err_t d1l_settings_load(void)
{
    if (!settings_owner_take()) {
        s_loaded = false;
        s_load_status = ESP_ERR_NO_MEM;
        s_persistence_state = D1L_SETTINGS_PERSISTENCE_IO_ERROR;
        return s_load_status;
    }
    const esp_err_t ret = settings_load_locked();
    settings_owner_give();
    return ret;
}

static esp_err_t settings_save_locked(const d1l_settings_t *settings)
{
    if (settings == NULL) {
        s_save_status = ESP_ERR_INVALID_ARG;
        return s_save_status;
    }
    if (s_persistence_write_blocked) {
        s_save_status = s_load_status;
        return s_save_status;
    }
    if (!s_loaded) {
        s_save_status = ESP_ERR_INVALID_STATE;
        return s_save_status;
    }
    if (settings->schema_version != D1L_SETTINGS_SCHEMA_VERSION) {
        s_save_status = ESP_ERR_NOT_SUPPORTED;
        return s_save_status;
    }

    uint32_t next_revision = 0U;
    if (!d1l_settings_envelope_next_revision(
            s_persistence_revision, &next_revision)) {
        s_persistence_state =
            D1L_SETTINGS_PERSISTENCE_REVISION_SATURATED;
        s_save_status = ESP_ERR_INVALID_STATE;
        return s_save_status;
    }

    d1l_settings_t copy = *settings;
    d1l_settings_sanitize(&copy);

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        wipe_sensitive_bytes(&copy, sizeof(copy));
        s_save_status = ret;
        return ret;
    }
    ret = persist_settings_envelope(handle, &copy, next_revision);
    nvs_close(handle);
    if (ret == ESP_OK) {
        s_current = copy;
        s_loaded = true;
        s_persistence_revision = next_revision;
        s_persistence_state = D1L_SETTINGS_PERSISTENCE_READY;
    }
    wipe_sensitive_bytes(&copy, sizeof(copy));
    s_save_status = ret;
    return ret;
}

static void apply_settings_update_fields(
    d1l_settings_t *destination, const d1l_settings_t *values,
    d1l_settings_update_mask_t update_mask)
{
    if ((update_mask & D1L_SETTINGS_UPDATE_NODE_NAME) != 0U) {
        memcpy(destination->node_name, values->node_name,
               sizeof(destination->node_name));
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_ROLE) != 0U) {
        destination->role = values->role;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_WIFI_ENABLED) != 0U) {
        destination->wifi_enabled = values->wifi_enabled;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_BLE_ENABLED) != 0U) {
        destination->ble_companion_enabled = values->ble_companion_enabled;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_OBSERVER) != 0U) {
        destination->observer_enabled = values->observer_enabled;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_HIGH_CONTRAST) != 0U) {
        destination->high_contrast = values->high_contrast;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_NIGHT_MODE) != 0U) {
        destination->night_mode = values->night_mode;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_ONBOARDING) != 0U) {
        destination->onboarding_complete = values->onboarding_complete;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_PATH_HASH) != 0U) {
        destination->path_hash_bytes = values->path_hash_bytes;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_FREQUENCY) != 0U) {
        destination->frequency_hz = values->frequency_hz;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_BANDWIDTH) != 0U) {
        destination->bandwidth_tenths_khz = values->bandwidth_tenths_khz;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_SPREADING) != 0U) {
        destination->spreading_factor = values->spreading_factor;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_CODING_RATE) != 0U) {
        destination->coding_rate = values->coding_rate;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_TX_POWER) != 0U) {
        destination->tx_power_dbm = values->tx_power_dbm;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_RX_BOOST) != 0U) {
        destination->rx_boost = values->rx_boost;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_TCXO) != 0U) {
        destination->tcxo_mode = values->tcxo_mode;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_MAP_LOCATION) != 0U) {
        destination->map_location_set = values->map_location_set;
        destination->map_lat_e7 = values->map_lat_e7;
        destination->map_lon_e7 = values->map_lon_e7;
        destination->map_location_source = values->map_location_source;
    }
    if ((update_mask & D1L_SETTINGS_UPDATE_TIMEZONE) != 0U) {
        destination->timezone_schema_version =
            values->timezone_schema_version;
        destination->timezone_offset_minutes =
            values->timezone_offset_minutes;
    }
}

static esp_err_t settings_replace_all(const d1l_settings_t *settings)
{
    if (!settings_owner_take()) {
        s_save_status = ESP_ERR_NO_MEM;
        return s_save_status;
    }
    const esp_err_t ret = settings_save_locked(settings);
    settings_owner_give();
    return ret;
}

esp_err_t d1l_settings_public_snapshot(d1l_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!settings_owner_take()) {
        d1l_settings_defaults(settings);
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t ret = s_loaded ? ESP_OK : s_load_status;
    if (s_loaded) {
        copy_public_settings(settings, &s_current);
    } else {
        d1l_settings_defaults(settings);
    }
    settings_owner_give();
    return ret;
}

void d1l_settings_wifi_secret_wipe(d1l_settings_wifi_secret_t *secret)
{
    wipe_sensitive_bytes(secret, secret ? sizeof(*secret) : 0U);
}

esp_err_t d1l_settings_wifi_secret_snapshot(
    d1l_settings_wifi_secret_t *secret)
{
    if (!secret) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_settings_wifi_secret_wipe(secret);
    if (!settings_owner_take()) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t ret = s_loaded ? ESP_OK : s_load_status;
    if (s_loaded) {
        secret->wifi_enabled = s_current.wifi_enabled;
        secret->wifi_profile_saved = s_current.wifi_profile_saved;
        secret->wifi_profile_count = s_current.wifi_profile_count;
        secret->wifi_active_profile = s_current.wifi_active_profile;
        memcpy(secret->wifi_ssid, s_current.wifi_ssid,
               sizeof(secret->wifi_ssid));
        memcpy(secret->wifi_password, s_current.wifi_password,
               sizeof(secret->wifi_password));
    }
    settings_owner_give();
    return ret;
}

bool d1l_settings_wifi_password_saved(void)
{
    if (!settings_owner_take()) {
        return false;
    }
    const bool saved = s_loaded && s_current.wifi_profile_saved &&
                       s_current.wifi_password[0] != '\0';
    settings_owner_give();
    return saved;
}

size_t d1l_settings_wifi_profiles_snapshot(
    d1l_wifi_profile_info_t *out_profiles, size_t max_profiles,
    uint8_t *out_active_profile)
{
    if (out_active_profile) {
        *out_active_profile = 0U;
    }
    if (!out_profiles || max_profiles == 0U) {
        return 0U;
    }
    const size_t clear_count =
        max_profiles < D1L_WIFI_PROFILE_CAPACITY ?
            max_profiles : D1L_WIFI_PROFILE_CAPACITY;
    memset(out_profiles, 0, clear_count * sizeof(out_profiles[0]));
    if (!settings_owner_take()) {
        return 0U;
    }
    size_t copied = 0U;
    if (s_loaded) {
        const size_t limit =
            s_current.wifi_profile_count < max_profiles ?
                s_current.wifi_profile_count : max_profiles;
        for (size_t i = 0U; i < limit; ++i) {
            out_profiles[i].saved =
                s_current.wifi_profiles[i].saved;
            out_profiles[i].password_saved =
                s_current.wifi_profiles[i].password[0] != '\0';
            snprintf(
                out_profiles[i].ssid,
                sizeof(out_profiles[i].ssid), "%s",
                s_current.wifi_profiles[i].ssid);
            copied++;
        }
        if (out_active_profile) {
            *out_active_profile = s_current.wifi_active_profile;
        }
    }
    settings_owner_give();
    return copied;
}

d1l_identity_state_t d1l_settings_persisted_identity_state(void)
{
    if (!settings_owner_take()) {
        return D1L_IDENTITY_STATE_INCONSISTENT;
    }
    const d1l_identity_state_t state = s_loaded ?
        d1l_settings_identity_state(&s_current) :
        D1L_IDENTITY_STATE_INCONSISTENT;
    settings_owner_give();
    return state;
}

void d1l_settings_identity_secret_wipe(
    d1l_settings_identity_secret_t *secret)
{
    wipe_sensitive_bytes(secret, secret ? sizeof(*secret) : 0U);
}

esp_err_t d1l_settings_identity_secret_snapshot(
    d1l_settings_identity_secret_t *secret)
{
    if (!secret) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_settings_identity_secret_wipe(secret);
    if (!settings_owner_take()) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t ret = s_loaded ? ESP_OK : s_load_status;
    if (s_loaded) {
        secret->identity_ready = s_current.identity_ready;
        memcpy(secret->identity_public_key, s_current.identity_public_key,
               sizeof(secret->identity_public_key));
        memcpy(secret->identity_private_key, s_current.identity_private_key,
               sizeof(secret->identity_private_key));
    }
    settings_owner_give();
    return ret;
}

esp_err_t d1l_settings_update_fields(
    const d1l_settings_t *settings, d1l_settings_update_mask_t update_mask)
{
    if (!settings || update_mask == 0U ||
        (update_mask & ~D1L_SETTINGS_UPDATE_ALL) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!settings_owner_take()) {
        s_save_status = ESP_ERR_NO_MEM;
        return s_save_status;
    }
    d1l_settings_t updated = s_current;
    apply_settings_update_fields(&updated, settings, update_mask);
    const esp_err_t ret = settings_save_locked(&updated);
    wipe_sensitive_bytes(&updated, sizeof(updated));
    settings_owner_give();
    return ret;
}

esp_err_t d1l_settings_save_identity_if_absent(
    const uint8_t public_key[D1L_IDENTITY_PUBLIC_KEY_LEN],
    const uint8_t private_key[D1L_IDENTITY_PRIVATE_KEY_LEN])
{
    if (!public_key || !private_key) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!settings_owner_take()) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_OK;
    const d1l_identity_state_t current_state =
        d1l_settings_identity_state(&s_current);
    if (!s_loaded) {
        ret = s_persistence_write_blocked ? s_load_status
                                          : ESP_ERR_INVALID_STATE;
    } else if (current_state == D1L_IDENTITY_STATE_INCONSISTENT) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (current_state == D1L_IDENTITY_STATE_ABSENT) {
        d1l_settings_t updated = s_current;
        updated.identity_ready = true;
        memcpy(updated.identity_public_key, public_key,
               sizeof(updated.identity_public_key));
        memcpy(updated.identity_private_key, private_key,
               sizeof(updated.identity_private_key));
        if (d1l_settings_identity_state(&updated) !=
            D1L_IDENTITY_STATE_CONSISTENT) {
            ret = ESP_ERR_INVALID_ARG;
        } else {
            ret = settings_save_locked(&updated);
        }
        wipe_sensitive_bytes(&updated, sizeof(updated));
    }
    settings_owner_give();
    return ret;
}

esp_err_t d1l_settings_reset(void)
{
    d1l_settings_t defaults;
    d1l_settings_defaults(&defaults);
    return settings_replace_all(&defaults);
}

esp_err_t d1l_settings_save_wifi_profile(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) >= D1L_WIFI_SSID_LEN ||
        (password && strlen(password) >= D1L_WIFI_PASSWORD_LEN)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!settings_owner_take()) {
        return ESP_ERR_NO_MEM;
    }
    d1l_settings_t settings = s_current;
    size_t profile_index = settings.wifi_profile_count;
    bool existing = false;
    for (size_t i = 0U; i < settings.wifi_profile_count; ++i) {
        if (strcmp(settings.wifi_profiles[i].ssid, ssid) == 0) {
            profile_index = i;
            existing = true;
            break;
        }
    }
    if (!existing &&
        settings.wifi_profile_count >= D1L_WIFI_PROFILE_CAPACITY) {
        wipe_sensitive_bytes(&settings, sizeof(settings));
        settings_owner_give();
        return ESP_ERR_NO_MEM;
    }
    d1l_wifi_profile_t *profile =
        &settings.wifi_profiles[profile_index];
    if (!existing) {
        wipe_sensitive_bytes(profile, sizeof(*profile));
        settings.wifi_profile_count++;
    }
    profile->saved = true;
    memset(profile->ssid, 0, sizeof(profile->ssid));
    snprintf(profile->ssid, sizeof(profile->ssid), "%s", ssid);
    if (password) {
        wipe_sensitive_bytes(
            profile->password, sizeof(profile->password));
        snprintf(
            profile->password, sizeof(profile->password), "%s",
            password);
    } else if (!existing) {
        wipe_sensitive_bytes(
            profile->password, sizeof(profile->password));
    }
    settings.wifi_active_profile = (uint8_t)profile_index;
    settings.wifi_profile_saved = true;
    const esp_err_t ret = settings_save_locked(&settings);
    wipe_sensitive_bytes(&settings, sizeof(settings));
    settings_owner_give();
    return ret;
}

esp_err_t d1l_settings_select_wifi_profile(uint8_t profile_index)
{
    if (!settings_owner_take()) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_loaded ||
        profile_index >= s_current.wifi_profile_count ||
        !s_current.wifi_profiles[profile_index].saved) {
        const esp_err_t ret =
            s_loaded ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
        settings_owner_give();
        return ret;
    }
    d1l_settings_t settings = s_current;
    settings.wifi_active_profile = profile_index;
    const esp_err_t ret = settings_save_locked(&settings);
    wipe_sensitive_bytes(&settings, sizeof(settings));
    settings_owner_give();
    return ret;
}

esp_err_t d1l_settings_delete_wifi_profile(uint8_t profile_index)
{
    if (!settings_owner_take()) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_loaded ||
        profile_index >= s_current.wifi_profile_count) {
        const esp_err_t ret =
            s_loaded ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
        settings_owner_give();
        return ret;
    }
    d1l_settings_t settings = s_current;
    const uint8_t previous_active = settings.wifi_active_profile;
    for (size_t i = profile_index;
         i + 1U < settings.wifi_profile_count; ++i) {
        settings.wifi_profiles[i] =
            settings.wifi_profiles[i + 1U];
    }
    wipe_sensitive_bytes(
        &settings.wifi_profiles[settings.wifi_profile_count - 1U],
        sizeof(settings.wifi_profiles[0]));
    settings.wifi_profile_count--;
    if (settings.wifi_profile_count == 0U) {
        settings.wifi_active_profile = 0U;
        settings.wifi_profile_saved = false;
        settings.wifi_enabled = false;
    } else if (previous_active > profile_index) {
        settings.wifi_active_profile =
            (uint8_t)(previous_active - 1U);
    } else if (previous_active == profile_index) {
        settings.wifi_active_profile =
            profile_index < settings.wifi_profile_count ?
                profile_index :
                (uint8_t)(settings.wifi_profile_count - 1U);
    }
    const esp_err_t ret = settings_save_locked(&settings);
    wipe_sensitive_bytes(&settings, sizeof(settings));
    settings_owner_give();
    return ret;
}

esp_err_t d1l_settings_clear_wifi_profile(void)
{
    if (!settings_owner_take()) {
        return ESP_ERR_NO_MEM;
    }
    d1l_settings_t settings = s_current;
    settings.wifi_profile_saved = false;
    memset(settings.wifi_ssid, 0, sizeof(settings.wifi_ssid));
    wipe_sensitive_bytes(settings.wifi_password,
                         sizeof(settings.wifi_password));
    settings.wifi_profile_count = 0U;
    settings.wifi_active_profile = 0U;
    wipe_sensitive_bytes(
        settings.wifi_profiles, sizeof(settings.wifi_profiles));
    settings.wifi_enabled = false;
    const esp_err_t ret = settings_save_locked(&settings);
    wipe_sensitive_bytes(&settings, sizeof(settings));
    settings_owner_give();
    return ret;
}

esp_err_t d1l_settings_complete_onboarding(const char *node_name, bool wifi_enabled,
                                           bool ble_companion_enabled, bool observer_enabled)
{
    if (!settings_owner_take()) {
        return ESP_ERR_NO_MEM;
    }
    d1l_settings_t settings = s_current;
    if (node_name && node_name[0] != '\0') {
        snprintf(settings.node_name, sizeof(settings.node_name), "%s", node_name);
    }
    settings.role = D1L_ROLE_DESK_COMPANION;
    settings.wifi_enabled = wifi_enabled;
    settings.ble_companion_enabled = wifi_enabled ? false : ble_companion_enabled;
    settings.observer_enabled = observer_enabled;
    settings.onboarding_complete = true;
    const esp_err_t ret = settings_save_locked(&settings);
    wipe_sensitive_bytes(&settings, sizeof(settings));
    settings_owner_give();
    return ret;
}

esp_err_t d1l_settings_reset_onboarding(void)
{
    if (!settings_owner_take()) {
        return ESP_ERR_NO_MEM;
    }
    d1l_settings_t settings = s_current;
    settings.onboarding_complete = false;
    const esp_err_t ret = settings_save_locked(&settings);
    wipe_sensitive_bytes(&settings, sizeof(settings));
    settings_owner_give();
    return ret;
}

esp_err_t d1l_settings_load_status(void)
{
    if (!settings_owner_take()) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t status = s_load_status;
    settings_owner_give();
    return status;
}

esp_err_t d1l_settings_save_status(void)
{
    if (!settings_owner_take()) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t status = s_save_status;
    settings_owner_give();
    return status;
}

uint32_t d1l_settings_persistence_revision(void)
{
    if (!settings_owner_take()) {
        return 0U;
    }
    const uint32_t revision = s_persistence_revision;
    settings_owner_give();
    return revision;
}

d1l_settings_persistence_state_t d1l_settings_persistence_state(void)
{
    if (!settings_owner_take()) {
        return D1L_SETTINGS_PERSISTENCE_IO_ERROR;
    }
    const d1l_settings_persistence_state_t state = s_persistence_state;
    settings_owner_give();
    return state;
}

const char *d1l_settings_persistence_state_name(
    d1l_settings_persistence_state_t state)
{
    switch (state) {
    case D1L_SETTINGS_PERSISTENCE_READY:
        return "ready";
    case D1L_SETTINGS_PERSISTENCE_INITIALIZED_DEFAULTS:
        return "initialized_defaults";
    case D1L_SETTINGS_PERSISTENCE_MIGRATED_LEGACY:
        return "migrated_legacy";
    case D1L_SETTINGS_PERSISTENCE_QUARANTINED_NEWER_SCHEMA:
        return "quarantined_newer_schema";
    case D1L_SETTINGS_PERSISTENCE_QUARANTINED_MALFORMED:
        return "quarantined_malformed";
    case D1L_SETTINGS_PERSISTENCE_QUARANTINED_CHECKSUM:
        return "quarantined_checksum";
    case D1L_SETTINGS_PERSISTENCE_IO_ERROR:
        return "io_error";
    case D1L_SETTINGS_PERSISTENCE_REVISION_SATURATED:
        return "revision_saturated";
    case D1L_SETTINGS_PERSISTENCE_UNINITIALIZED:
    default:
        return "uninitialized";
    }
}

d1l_radio_profile_t d1l_settings_radio_profile(const d1l_settings_t *settings)
{
    d1l_settings_t snapshot = {0};
    const d1l_settings_t *src = settings;
    if (!src) {
        (void)d1l_settings_public_snapshot(&snapshot);
        src = &snapshot;
    }
    d1l_radio_profile_t profile = {
        .profile_id = D1L_RADIO_PROFILE_ID,
        .region_label = D1L_RADIO_REGION_LABEL,
        .frequency_hz = src->frequency_hz,
        .bandwidth_khz = ((float)src->bandwidth_tenths_khz) / 10.0f,
        .spreading_factor = src->spreading_factor,
        .coding_rate = src->coding_rate,
        .tx_power_dbm = src->tx_power_dbm,
        .tcxo = d1l_settings_tcxo_name(src->tcxo_mode),
        .rx_boost = src->rx_boost,
    };
    wipe_sensitive_bytes(&snapshot, sizeof(snapshot));
    return profile;
}

const char *d1l_settings_role_name(uint8_t role)
{
    switch (role) {
    case D1L_ROLE_DESK_COMPANION:
        return "desk_companion";
    default:
        return "unknown";
    }
}

const char *d1l_settings_tcxo_name(uint8_t tcxo_mode)
{
    switch (tcxo_mode) {
    case D1L_TCXO_NONE:
        return "NONE";
    default:
        return "NONE";
    }
}
