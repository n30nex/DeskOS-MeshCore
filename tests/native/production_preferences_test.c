#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app/settings_model.h"
#include "hal/display_preferences.h"
#include "mesh/contact_policy.h"
#include "mock_esp_nvs.h"
#include "freertos/semphr.h"

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static StaticSemaphore_t mutex;
    return xSemaphoreCreateMutexStatic(&mutex);
}

static uint8_t owner = 42U;
esp_err_t d1l_settings_public_snapshot(d1l_settings_t *out)
{
    *out = (d1l_settings_t){.identity_ready = true};
    out->identity_public_key[0] = owner;
    return ESP_OK;
}

int main(int argc, char **argv)
{
    mock_nvs_reset();
    const uint8_t brightness = 55U, notification = D1L_NOTIFICATION_MODE_QUIET_HOURS;
    const uint16_t timeout = 300U;
    assert(mock_nvs_seed_blob("d1l_display", "brightness", &brightness, sizeof(brightness)));
    assert(mock_nvs_seed_blob("d1l_display", "timeout", &timeout, sizeof(timeout)));
    assert(mock_nvs_seed_blob("d1l_display", "notify", &notification, sizeof(notification)));
    const uint8_t stored_layout = argc > 1 && strcmp(argv[1], "invalid") == 0 ? 255U : 2U;
    if (argc > 1) assert(mock_nvs_seed_blob("d1l_display", "keyboard", &stored_layout, sizeof(stored_layout)));
    assert(d1l_display_preferences_init() == ESP_OK);
    d1l_display_preferences_t display;
    d1l_display_preferences_get(&display);
    assert(display.brightness_percent == 55U && display.timeout_seconds == 300U);
    assert(display.notification_mode == D1L_NOTIFICATION_MODE_QUIET_HOURS);
    if (argc > 1) {
        assert(display.keyboard_layout == (stored_layout == 255U ? 0U : 2U));
        puts("stored keyboard preference: ok");
        return 0;
    }
    assert(display.text_size == 0U && display.daylight_saving == D1L_DAYLIGHT_SAVING_OFF);
    mock_nvs_fail_next_set(ESP_FAIL);
    assert(d1l_display_preferences_set_text_size(1U) == ESP_FAIL);
    d1l_display_preferences_get(&display); assert(display.text_size == 0U);
    assert(d1l_display_preferences_set_text_size(1U) == ESP_OK);
    uint8_t value;
    assert(display.keyboard_layout == 0U);
    assert(d1l_display_preferences_set_keyboard_layout(D1L_KEYBOARD_LAYOUT_COUNT) == ESP_ERR_INVALID_ARG);
    mock_nvs_fail_next_set(ESP_FAIL);
    assert(d1l_display_preferences_set_keyboard_layout(1U) == ESP_FAIL);
    d1l_display_preferences_get(&display); assert(display.keyboard_layout == 0U);
    assert(d1l_display_preferences_set_keyboard_layout(1U) == ESP_OK);
    mock_nvs_fail_next_commit(ESP_FAIL);
    assert(d1l_display_preferences_set_keyboard_layout(2U) == ESP_FAIL);
    d1l_display_preferences_get(&display); assert(display.keyboard_layout == 1U);
    assert(mock_nvs_copy_blob("d1l_display", "keyboard", &value, sizeof(value)) == 1U && value == 1U);
    assert(strcmp(d1l_keyboard_layout_name(1U), "AZERTY") == 0);
    assert(strcmp(d1l_keyboard_layout_name(2U), "QWERTZ") == 0);
    assert(mock_nvs_copy_blob("d1l_display", "text_size", &value, sizeof(value)) == 1U && value == 1U);
    mock_nvs_fail_next_commit(ESP_FAIL);
    assert(d1l_display_preferences_set_daylight_saving(D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == ESP_FAIL);
    d1l_display_preferences_get(&display); assert(display.daylight_saving == D1L_DAYLIGHT_SAVING_OFF);
    assert(d1l_display_preferences_set_daylight_saving(D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == ESP_OK);
    assert(mock_nvs_copy_blob("d1l_display", "dst", &value, sizeof(value)) == 1U && value == 1U);
    assert(mock_nvs_copy_blob("d1l_display", "brightness", &value, sizeof(value)) == 1U && value == 55U);
    assert(d1l_display_preferences_set_text_size(2U) == ESP_ERR_INVALID_ARG);
    assert(d1l_display_preferences_set_daylight_saving((d1l_daylight_saving_t)3) == ESP_ERR_INVALID_ARG);

    assert(!d1l_contact_policy_allows(1U, 0U));
    assert(d1l_contact_policy_init() == ESP_OK);
    assert(!d1l_contact_policy_get().manual && d1l_contact_policy_allows(1U, 63U));
    d1l_contact_policy_t policy = {.roles = 2U, .max_hops = 1U, .manual = true};
    assert(d1l_contact_policy_save(policy) == ESP_OK);
    assert(d1l_contact_policy_allows(1U, 0U));
    assert(!d1l_contact_policy_allows(1U, 1U));
    assert(!d1l_contact_policy_allows(2U, 0U));
    assert(!d1l_contact_policy_allows(255U, 0U));
    assert(d1l_contact_policy_init() == ESP_OK && d1l_contact_policy_get().manual);
    for (uint8_t type = 1U; type <= 4U; ++type) {
        policy.roles = (uint8_t)(1U << type);
        assert(d1l_contact_policy_save(policy) == ESP_OK);
        for (uint8_t peer = 1U; peer <= 4U; ++peer) assert(d1l_contact_policy_allows(peer, 0U) == (peer == type));
    }
    mock_nvs_fail_next_commit(ESP_FAIL);
    policy.roles = 0U;
    assert(d1l_contact_policy_save(policy) == ESP_FAIL);
    assert(d1l_contact_policy_get().roles == 16U);
    policy.roles = 1U; assert(d1l_contact_policy_save(policy) == ESP_ERR_INVALID_ARG);
    policy.roles = 30U; policy.max_hops = 65U;
    assert(d1l_contact_policy_save(policy) == ESP_ERR_INVALID_ARG);
    ++owner;
    assert(d1l_contact_policy_init() == ESP_OK && !d1l_contact_policy_get().manual);
    mock_nvs_fail_next_open(ESP_FAIL);
    assert(d1l_contact_policy_init() == ESP_FAIL);
    assert(!d1l_contact_policy_allows(1U, 0U));
    puts("production preferences: ok");
    return 0;
}
