#include "release_profile.h"

_Static_assert(D1L_RELEASE_PROFILE >= D1L_RELEASE_PROFILE_DEVELOPMENT &&
                   D1L_RELEASE_PROFILE <= D1L_RELEASE_PROFILE_FULL_FEATURE,
               "unsupported D1L release profile");
_Static_assert(D1L_SD_HISTORY_MODE >= D1L_SD_HISTORY_MODE_DISABLED &&
                   D1L_SD_HISTORY_MODE <=
                       D1L_SD_HISTORY_MODE_SUPPORTED_OPTIONAL,
               "unsupported D1L SD history mode");

static const d1l_release_capabilities_t s_core_capabilities = {
    .board_display_touch_backlight = true,
    .home_core_navigation = true,
    .public_messages = true,
    .direct_messages = true,
    .basic_contacts = true,
    .nodes = true,
    .packets = true,
    .route_signal_read_only = true,
    .radio_settings = true,
    .identity = true,
    .retained_nvs = true,
    .sd_history =
        D1L_SD_HISTORY_MODE == D1L_SD_HISTORY_MODE_SUPPORTED_OPTIONAL,
    .diagnostics = true,
    .usb_recovery = true,
    .time_truth = true,
    .map = false,
    .wifi_user_control = false,
    .ble = false,
    .multi_channel_management = false,
    .admin = false,
    .observer_mqtt = false,
    .signed_update = false,
    .mutable_terminal = false,
    .location = false,
    .advanced_qr_emoji = false,
    .user_trace = false,
    .sd_history_mode = D1L_SD_HISTORY_MODE,
};

static const d1l_release_capabilities_t s_development_capabilities = {
    .board_display_touch_backlight = true,
    .home_core_navigation = true,
    .public_messages = true,
    .direct_messages = true,
    .basic_contacts = true,
    .nodes = true,
    .packets = true,
    .route_signal_read_only = true,
    .radio_settings = true,
    .identity = true,
    .retained_nvs = true,
    .sd_history =
        D1L_SD_HISTORY_MODE != D1L_SD_HISTORY_MODE_DISABLED,
    .diagnostics = true,
    .usb_recovery = true,
    .time_truth = true,
    .map = true,
    .wifi_user_control = true,
    .ble = true,
    .multi_channel_management = true,
    .admin = true,
    .observer_mqtt = true,
    .signed_update = true,
    .mutable_terminal = true,
    .location = true,
    .advanced_qr_emoji = true,
    .user_trace = true,
    .sd_history_mode = D1L_SD_HISTORY_MODE,
};

static const d1l_release_capabilities_t s_full_feature_capabilities = {
    .board_display_touch_backlight = true,
    .home_core_navigation = true,
    .public_messages = true,
    .direct_messages = true,
    .basic_contacts = true,
    .nodes = true,
    .packets = true,
    .route_signal_read_only = true,
    .radio_settings = true,
    .identity = true,
    .retained_nvs = true,
    .sd_history =
        D1L_SD_HISTORY_MODE != D1L_SD_HISTORY_MODE_DISABLED,
    .diagnostics = true,
    .usb_recovery = true,
    .time_truth = true,
    .map = true,
    .wifi_user_control = true,
    .ble = true,
    .multi_channel_management = true,
    .admin = true,
    .observer_mqtt = true,
    .signed_update = true,
    .mutable_terminal = true,
    .location = true,
    .advanced_qr_emoji = true,
    .user_trace = true,
    .sd_history_mode = D1L_SD_HISTORY_MODE,
};

static const char *const s_feature_names[D1L_RELEASE_FEATURE_COUNT] = {
    [D1L_RELEASE_FEATURE_PUBLIC_MESSAGES] = "public_messages",
    [D1L_RELEASE_FEATURE_DIRECT_MESSAGES] = "direct_messages",
    [D1L_RELEASE_FEATURE_BASIC_CONTACTS] = "basic_contacts",
    [D1L_RELEASE_FEATURE_NODES] = "nodes",
    [D1L_RELEASE_FEATURE_PACKETS] = "packets",
    [D1L_RELEASE_FEATURE_ROUTE_SIGNAL_READ_ONLY] = "route_signal_read_only",
    [D1L_RELEASE_FEATURE_RADIO_SETTINGS] = "radio_settings",
    [D1L_RELEASE_FEATURE_IDENTITY] = "identity",
    [D1L_RELEASE_FEATURE_RETAINED_NVS] = "retained_nvs",
    [D1L_RELEASE_FEATURE_SD_HISTORY] = "sd_history",
    [D1L_RELEASE_FEATURE_DIAGNOSTICS] = "diagnostics",
    [D1L_RELEASE_FEATURE_USB_RECOVERY] = "usb_recovery",
    [D1L_RELEASE_FEATURE_TIME_TRUTH] = "time_truth",
    [D1L_RELEASE_FEATURE_MAP] = "map",
    [D1L_RELEASE_FEATURE_WIFI_USER_CONTROL] = "wifi_user_control",
    [D1L_RELEASE_FEATURE_BLE] = "ble",
    [D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT] =
        "multi_channel_management",
    [D1L_RELEASE_FEATURE_ADMIN] = "admin",
    [D1L_RELEASE_FEATURE_OBSERVER_MQTT] = "observer_mqtt",
    [D1L_RELEASE_FEATURE_SIGNED_UPDATE] = "signed_update",
    [D1L_RELEASE_FEATURE_MUTABLE_TERMINAL] = "mutable_terminal",
    [D1L_RELEASE_FEATURE_LOCATION] = "location",
    [D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI] = "advanced_qr_emoji",
    [D1L_RELEASE_FEATURE_USER_TRACE] = "user_trace",
};

d1l_release_profile_id_t d1l_release_profile_id(void)
{
    return (d1l_release_profile_id_t)D1L_RELEASE_PROFILE;
}

const char *d1l_release_profile_name(void)
{
    switch (d1l_release_profile_id()) {
    case D1L_RELEASE_PROFILE_DEVELOPMENT:
        return "development";
    case D1L_RELEASE_PROFILE_CORE_1_0:
        return "core_1_0";
    case D1L_RELEASE_PROFILE_FULL_FEATURE:
        return "full_feature";
    default:
        return "invalid";
    }
}

bool d1l_release_profile_is_core(void)
{
    return d1l_release_profile_id() == D1L_RELEASE_PROFILE_CORE_1_0;
}

d1l_sd_history_mode_t d1l_release_sd_history_mode(void)
{
    return (d1l_sd_history_mode_t)D1L_SD_HISTORY_MODE;
}

const char *d1l_release_sd_history_mode_name(void)
{
    switch (d1l_release_sd_history_mode()) {
    case D1L_SD_HISTORY_MODE_DISABLED:
        return "disabled";
    case D1L_SD_HISTORY_MODE_CONDITIONAL:
        return "conditional";
    case D1L_SD_HISTORY_MODE_SUPPORTED_OPTIONAL:
        return "supported_optional";
    default:
        return "invalid";
    }
}

const d1l_release_capabilities_t *d1l_release_capabilities(void)
{
    switch (d1l_release_profile_id()) {
    case D1L_RELEASE_PROFILE_CORE_1_0:
        return &s_core_capabilities;
    case D1L_RELEASE_PROFILE_FULL_FEATURE:
        return &s_full_feature_capabilities;
    case D1L_RELEASE_PROFILE_DEVELOPMENT:
    default:
        return &s_development_capabilities;
    }
}

bool d1l_release_feature_available(d1l_release_feature_id_t feature)
{
    const d1l_release_capabilities_t *capabilities =
        d1l_release_capabilities();
    switch (feature) {
    case D1L_RELEASE_FEATURE_PUBLIC_MESSAGES:
        return capabilities->public_messages;
    case D1L_RELEASE_FEATURE_DIRECT_MESSAGES:
        return capabilities->direct_messages;
    case D1L_RELEASE_FEATURE_BASIC_CONTACTS:
        return capabilities->basic_contacts;
    case D1L_RELEASE_FEATURE_NODES:
        return capabilities->nodes;
    case D1L_RELEASE_FEATURE_PACKETS:
        return capabilities->packets;
    case D1L_RELEASE_FEATURE_ROUTE_SIGNAL_READ_ONLY:
        return capabilities->route_signal_read_only;
    case D1L_RELEASE_FEATURE_RADIO_SETTINGS:
        return capabilities->radio_settings;
    case D1L_RELEASE_FEATURE_IDENTITY:
        return capabilities->identity;
    case D1L_RELEASE_FEATURE_RETAINED_NVS:
        return capabilities->retained_nvs;
    case D1L_RELEASE_FEATURE_SD_HISTORY:
        return capabilities->sd_history;
    case D1L_RELEASE_FEATURE_DIAGNOSTICS:
        return capabilities->diagnostics;
    case D1L_RELEASE_FEATURE_USB_RECOVERY:
        return capabilities->usb_recovery;
    case D1L_RELEASE_FEATURE_TIME_TRUTH:
        return capabilities->time_truth;
    case D1L_RELEASE_FEATURE_MAP:
        return capabilities->map;
    case D1L_RELEASE_FEATURE_WIFI_USER_CONTROL:
        return capabilities->wifi_user_control;
    case D1L_RELEASE_FEATURE_BLE:
        return capabilities->ble;
    case D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT:
        return capabilities->multi_channel_management;
    case D1L_RELEASE_FEATURE_ADMIN:
        return capabilities->admin;
    case D1L_RELEASE_FEATURE_OBSERVER_MQTT:
        return capabilities->observer_mqtt;
    case D1L_RELEASE_FEATURE_SIGNED_UPDATE:
        return capabilities->signed_update;
    case D1L_RELEASE_FEATURE_MUTABLE_TERMINAL:
        return capabilities->mutable_terminal;
    case D1L_RELEASE_FEATURE_LOCATION:
        return capabilities->location;
    case D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI:
        return capabilities->advanced_qr_emoji;
    case D1L_RELEASE_FEATURE_USER_TRACE:
        return capabilities->user_trace;
    case D1L_RELEASE_FEATURE_COUNT:
    default:
        return false;
    }
}

const char *d1l_release_feature_name(d1l_release_feature_id_t feature)
{
    if (feature < 0 || feature >= D1L_RELEASE_FEATURE_COUNT) {
        return "unknown";
    }
    return s_feature_names[feature];
}

size_t d1l_release_feature_count(void)
{
    return D1L_RELEASE_FEATURE_COUNT;
}
