#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    D1L_RELEASE_PROFILE_DEVELOPMENT = 0,
    D1L_RELEASE_PROFILE_CORE_1_0 = 1,
    D1L_RELEASE_PROFILE_FULL_FEATURE = 2,
} d1l_release_profile_id_t;

typedef enum {
    D1L_SD_HISTORY_MODE_DISABLED = 0,
    D1L_SD_HISTORY_MODE_CONDITIONAL = 1,
    D1L_SD_HISTORY_MODE_SUPPORTED_OPTIONAL = 2,
} d1l_sd_history_mode_t;

typedef enum {
    D1L_RELEASE_FEATURE_PUBLIC_MESSAGES = 0,
    D1L_RELEASE_FEATURE_DIRECT_MESSAGES,
    D1L_RELEASE_FEATURE_BASIC_CONTACTS,
    D1L_RELEASE_FEATURE_NODES,
    D1L_RELEASE_FEATURE_PACKETS,
    D1L_RELEASE_FEATURE_ROUTE_SIGNAL_READ_ONLY,
    D1L_RELEASE_FEATURE_RADIO_SETTINGS,
    D1L_RELEASE_FEATURE_IDENTITY,
    D1L_RELEASE_FEATURE_RETAINED_NVS,
    D1L_RELEASE_FEATURE_SD_HISTORY,
    D1L_RELEASE_FEATURE_DIAGNOSTICS,
    D1L_RELEASE_FEATURE_USB_RECOVERY,
    D1L_RELEASE_FEATURE_TIME_TRUTH,
    D1L_RELEASE_FEATURE_MAP,
    D1L_RELEASE_FEATURE_WIFI_USER_CONTROL,
    D1L_RELEASE_FEATURE_BLE,
    D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT,
    D1L_RELEASE_FEATURE_ADMIN,
    D1L_RELEASE_FEATURE_OBSERVER_MQTT,
    D1L_RELEASE_FEATURE_SIGNED_UPDATE,
    D1L_RELEASE_FEATURE_MUTABLE_TERMINAL,
    D1L_RELEASE_FEATURE_LOCATION,
    D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI,
    D1L_RELEASE_FEATURE_USER_TRACE,
    D1L_RELEASE_FEATURE_COUNT,
} d1l_release_feature_id_t;

typedef struct {
    bool board_display_touch_backlight;
    bool home_core_navigation;
    bool public_messages;
    bool direct_messages;
    bool basic_contacts;
    bool nodes;
    bool packets;
    bool route_signal_read_only;
    bool radio_settings;
    bool identity;
    bool retained_nvs;
    bool sd_history;
    bool diagnostics;
    bool usb_recovery;
    bool time_truth;
    bool map;
    bool wifi_user_control;
    bool ble;
    bool multi_channel_management;
    bool admin;
    bool observer_mqtt;
    bool signed_update;
    bool mutable_terminal;
    bool location;
    bool advanced_qr_emoji;
    bool user_trace;
    d1l_sd_history_mode_t sd_history_mode;
} d1l_release_capabilities_t;

#ifndef D1L_RELEASE_PROFILE
#define D1L_RELEASE_PROFILE D1L_RELEASE_PROFILE_DEVELOPMENT
#endif

#ifndef D1L_SD_HISTORY_MODE
#define D1L_SD_HISTORY_MODE D1L_SD_HISTORY_MODE_DISABLED
#endif

d1l_release_profile_id_t d1l_release_profile_id(void);
const char *d1l_release_profile_name(void);
bool d1l_release_profile_is_core(void);

d1l_sd_history_mode_t d1l_release_sd_history_mode(void);
const char *d1l_release_sd_history_mode_name(void);

const d1l_release_capabilities_t *d1l_release_capabilities(void);
bool d1l_release_feature_available(d1l_release_feature_id_t feature);
const char *d1l_release_feature_name(d1l_release_feature_id_t feature);
size_t d1l_release_feature_count(void);
