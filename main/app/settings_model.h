#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "identity_state.h"
#include "mesh/meshcore_radio_profile.h"
#include "platform/time_display.h"
#include "storage/map_tile_store.h"

#define D1L_SETTINGS_SCHEMA_VERSION 9U
#define D1L_NODE_NAME_LEN 32U
#define D1L_WIFI_SSID_LEN 33U
#define D1L_WIFI_PASSWORD_LEN 65U
#define D1L_IDENTITY_PUBLIC_KEY_LEN 32U
#define D1L_IDENTITY_PRIVATE_KEY_LEN 64U
#define D1L_MAP_LOCATION_LAT_E7_MIN (-900000000L)
#define D1L_MAP_LOCATION_LAT_E7_MAX 900000000L
#define D1L_MAP_LOCATION_LON_E7_MIN (-1800000000L)
#define D1L_MAP_LOCATION_LON_E7_MAX 1800000000L
#define D1L_MAP_TILE_DEFAULT_ZOOM 10U

typedef enum {
    D1L_MAP_LOCATION_SOURCE_UNKNOWN = 0,
    D1L_MAP_LOCATION_SOURCE_MANUAL = 1,
    D1L_MAP_LOCATION_SOURCE_AUTHENTICATED_COMPANION = 2,
} d1l_map_location_source_t;

typedef enum {
    D1L_ROLE_DESK_COMPANION = 0,
} d1l_role_t;

typedef enum {
    D1L_TCXO_NONE = 0,
} d1l_tcxo_mode_t;

typedef enum {
    D1L_SETTINGS_PERSISTENCE_UNINITIALIZED = 0,
    D1L_SETTINGS_PERSISTENCE_READY,
    D1L_SETTINGS_PERSISTENCE_INITIALIZED_DEFAULTS,
    D1L_SETTINGS_PERSISTENCE_MIGRATED_LEGACY,
    D1L_SETTINGS_PERSISTENCE_QUARANTINED_NEWER_SCHEMA,
    D1L_SETTINGS_PERSISTENCE_QUARANTINED_MALFORMED,
    D1L_SETTINGS_PERSISTENCE_QUARANTINED_CHECKSUM,
    D1L_SETTINGS_PERSISTENCE_IO_ERROR,
    D1L_SETTINGS_PERSISTENCE_REVISION_SATURATED,
} d1l_settings_persistence_state_t;

typedef uint32_t d1l_settings_update_mask_t;

#define D1L_SETTINGS_UPDATE_NODE_NAME       (1UL << 0)
#define D1L_SETTINGS_UPDATE_ROLE            (1UL << 1)
#define D1L_SETTINGS_UPDATE_WIFI_ENABLED    (1UL << 2)
#define D1L_SETTINGS_UPDATE_BLE_ENABLED     (1UL << 3)
#define D1L_SETTINGS_UPDATE_OBSERVER        (1UL << 4)
#define D1L_SETTINGS_UPDATE_HIGH_CONTRAST   (1UL << 5)
#define D1L_SETTINGS_UPDATE_NIGHT_MODE      (1UL << 6)
#define D1L_SETTINGS_UPDATE_ONBOARDING      (1UL << 7)
/* Bit 8 is reserved: Wi-Fi credentials use the narrow profile APIs. */
#define D1L_SETTINGS_UPDATE_PATH_HASH       (1UL << 9)
#define D1L_SETTINGS_UPDATE_FREQUENCY       (1UL << 10)
#define D1L_SETTINGS_UPDATE_BANDWIDTH       (1UL << 11)
#define D1L_SETTINGS_UPDATE_SPREADING       (1UL << 12)
#define D1L_SETTINGS_UPDATE_CODING_RATE     (1UL << 13)
#define D1L_SETTINGS_UPDATE_TX_POWER        (1UL << 14)
#define D1L_SETTINGS_UPDATE_RX_BOOST        (1UL << 15)
#define D1L_SETTINGS_UPDATE_TCXO            (1UL << 16)
#define D1L_SETTINGS_UPDATE_MAP_LOCATION    (1UL << 17)
/* Bit 18 is reserved: identity secrets use save_identity_if_absent(). */
#define D1L_SETTINGS_UPDATE_TIMEZONE        (1UL << 19)
#define D1L_SETTINGS_UPDATE_ALL \
    (D1L_SETTINGS_UPDATE_NODE_NAME | D1L_SETTINGS_UPDATE_ROLE | \
     D1L_SETTINGS_UPDATE_WIFI_ENABLED | D1L_SETTINGS_UPDATE_BLE_ENABLED | \
     D1L_SETTINGS_UPDATE_OBSERVER | D1L_SETTINGS_UPDATE_HIGH_CONTRAST | \
     D1L_SETTINGS_UPDATE_NIGHT_MODE | D1L_SETTINGS_UPDATE_ONBOARDING | \
     D1L_SETTINGS_UPDATE_PATH_HASH | D1L_SETTINGS_UPDATE_FREQUENCY | \
     D1L_SETTINGS_UPDATE_BANDWIDTH | D1L_SETTINGS_UPDATE_SPREADING | \
     D1L_SETTINGS_UPDATE_CODING_RATE | D1L_SETTINGS_UPDATE_TX_POWER | \
     D1L_SETTINGS_UPDATE_RX_BOOST | D1L_SETTINGS_UPDATE_TCXO | \
     D1L_SETTINGS_UPDATE_MAP_LOCATION | D1L_SETTINGS_UPDATE_TIMEZONE)
#define D1L_SETTINGS_UPDATE_RADIO_PROFILE \
    (D1L_SETTINGS_UPDATE_FREQUENCY | D1L_SETTINGS_UPDATE_BANDWIDTH | \
     D1L_SETTINGS_UPDATE_SPREADING | D1L_SETTINGS_UPDATE_CODING_RATE | \
     D1L_SETTINGS_UPDATE_TX_POWER | D1L_SETTINGS_UPDATE_RX_BOOST | \
     D1L_SETTINGS_UPDATE_TCXO)

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
} d1l_settings_t;

typedef struct {
    bool wifi_enabled;
    bool wifi_profile_saved;
    char wifi_ssid[D1L_WIFI_SSID_LEN];
    char wifi_password[D1L_WIFI_PASSWORD_LEN];
} d1l_settings_wifi_secret_t;

typedef struct {
    bool identity_ready;
    uint8_t identity_public_key[D1L_IDENTITY_PUBLIC_KEY_LEN];
    uint8_t identity_private_key[D1L_IDENTITY_PRIVATE_KEY_LEN];
} d1l_settings_identity_secret_t;

void d1l_settings_defaults(d1l_settings_t *settings);
void d1l_settings_sanitize(d1l_settings_t *settings);
d1l_identity_state_t d1l_settings_identity_state(const d1l_settings_t *settings);
esp_err_t d1l_settings_load(void);
/* Public snapshots always zero password and private-key fields. */
esp_err_t d1l_settings_public_snapshot(d1l_settings_t *settings);
esp_err_t d1l_settings_wifi_secret_snapshot(
    d1l_settings_wifi_secret_t *secret);
void d1l_settings_wifi_secret_wipe(d1l_settings_wifi_secret_t *secret);
bool d1l_settings_wifi_password_saved(void);
d1l_identity_state_t d1l_settings_persisted_identity_state(void);
esp_err_t d1l_settings_identity_secret_snapshot(
    d1l_settings_identity_secret_t *secret);
void d1l_settings_identity_secret_wipe(
    d1l_settings_identity_secret_t *secret);
esp_err_t d1l_settings_update_fields(
    const d1l_settings_t *settings, d1l_settings_update_mask_t update_mask);
esp_err_t d1l_settings_save_identity_if_absent(
    const uint8_t public_key[D1L_IDENTITY_PUBLIC_KEY_LEN],
    const uint8_t private_key[D1L_IDENTITY_PRIVATE_KEY_LEN]);
esp_err_t d1l_settings_reset(void);
esp_err_t d1l_settings_save_wifi_profile(const char *ssid, const char *password);
esp_err_t d1l_settings_clear_wifi_profile(void);
esp_err_t d1l_settings_complete_onboarding(const char *node_name, bool wifi_enabled,
                                           bool ble_companion_enabled, bool observer_enabled);
esp_err_t d1l_settings_reset_onboarding(void);
/* Compatibility alias implemented by the truthful time service. */
esp_err_t d1l_settings_next_mesh_timestamp(uint32_t *timestamp);
esp_err_t d1l_settings_load_status(void);
esp_err_t d1l_settings_save_status(void);
uint32_t d1l_settings_persistence_revision(void);
d1l_settings_persistence_state_t d1l_settings_persistence_state(void);
const char *d1l_settings_persistence_state_name(
    d1l_settings_persistence_state_t state);
d1l_radio_profile_t d1l_settings_radio_profile(const d1l_settings_t *settings);
const char *d1l_settings_role_name(uint8_t role);
const char *d1l_settings_tcxo_name(uint8_t tcxo_mode);
