#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"

int main(void)
{
    assert(d1l_release_feature_count() == D1L_RELEASE_FEATURE_COUNT);
    assert(strcmp(d1l_release_feature_name(D1L_RELEASE_FEATURE_MAP), "map") ==
           0);
    assert(strcmp(d1l_release_feature_name(D1L_RELEASE_FEATURE_USER_TRACE),
                  "user_trace") == 0);
    assert(strcmp(d1l_release_feature_name(D1L_RELEASE_FEATURE_COUNT),
                  "unknown") == 0);

#if EXPECT_CORE
    assert(d1l_release_profile_id() == D1L_RELEASE_PROFILE_CORE_1_0);
    assert(strcmp(d1l_release_profile_name(), "core_1_0") == 0);
    assert(d1l_release_profile_is_core());
    assert(d1l_release_feature_available(
        D1L_RELEASE_FEATURE_PUBLIC_MESSAGES));
    assert(d1l_release_feature_available(
        D1L_RELEASE_FEATURE_DIRECT_MESSAGES));
    assert(d1l_release_feature_available(D1L_RELEASE_FEATURE_NODES));
    assert(d1l_release_feature_available(D1L_RELEASE_FEATURE_PACKETS));
    assert(!d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP));
    assert(!d1l_release_feature_available(
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL));
    assert(!d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE));
    assert(!d1l_release_feature_available(
        D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT));
    assert(!d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN));
    assert(!d1l_release_feature_available(
        D1L_RELEASE_FEATURE_OBSERVER_MQTT));
    assert(!d1l_release_feature_available(
        D1L_RELEASE_FEATURE_SIGNED_UPDATE));
    assert(!d1l_release_feature_available(
        D1L_RELEASE_FEATURE_MUTABLE_TERMINAL));
    assert(!d1l_release_feature_available(D1L_RELEASE_FEATURE_LOCATION));
    assert(!d1l_release_feature_available(
        D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI));
    assert(!d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE));
#elif EXPECT_FULL
    assert(d1l_release_profile_id() == D1L_RELEASE_PROFILE_FULL_FEATURE);
    assert(strcmp(d1l_release_profile_name(), "full_feature") == 0);
    assert(!d1l_release_profile_is_core());
    assert(d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP));
    assert(d1l_release_feature_available(
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL));
    assert(d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN));
#else
    assert(d1l_release_profile_id() == D1L_RELEASE_PROFILE_DEVELOPMENT);
    assert(strcmp(d1l_release_profile_name(), "development") == 0);
    assert(!d1l_release_profile_is_core());
    assert(d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP));
    assert(d1l_release_feature_available(
        D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT));
#endif

#if EXPECT_SD_SUPPORTED
    assert(d1l_release_sd_history_mode() ==
           D1L_SD_HISTORY_MODE_SUPPORTED_OPTIONAL);
    assert(strcmp(d1l_release_sd_history_mode_name(),
                  "supported_optional") == 0);
    assert(d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY));
#elif EXPECT_SD_CONDITIONAL
    assert(d1l_release_sd_history_mode() ==
           D1L_SD_HISTORY_MODE_CONDITIONAL);
    assert(strcmp(d1l_release_sd_history_mode_name(), "conditional") == 0);
#if EXPECT_CORE
    assert(!d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY));
#endif
#else
    assert(d1l_release_sd_history_mode() ==
           D1L_SD_HISTORY_MODE_DISABLED);
    assert(strcmp(d1l_release_sd_history_mode_name(), "disabled") == 0);
    assert(!d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY));
#endif

    puts("native release profile: ok");
    return 0;
}
