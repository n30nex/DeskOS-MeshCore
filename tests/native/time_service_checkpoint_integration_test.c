#ifdef time
#undef time
#endif

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app/settings_model.h"
#include "app/settings_envelope.h"
#include "app/settings_protocol_migration.h"
#include "app/settings_time_checkpoint.h"
#include "esp_netif_sntp.h"
#include "mock_esp_nvs.h"
#include "platform/time_service.h"
#include "hal/display_preferences.h"

#define TEST_BUILD_EPOCH INT64_C(1784068276)
#define CHECKPOINT_NAMESPACE "d1l_time"
#define CHECKPOINT_KEY "wall_ckpt_v1"

static esp_err_t s_sntp_init_result = ESP_OK;
static esp_err_t s_sntp_wait_result = ESP_OK;
static bool s_sntp_wait_enabled;
static int16_t s_timezone_offset;
static d1l_daylight_saving_t s_daylight_saving;

esp_err_t d1l_settings_public_snapshot(d1l_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(settings, 0, sizeof(*settings));
    settings->timezone_schema_version = D1L_TIMEZONE_SETTING_SCHEMA_VERSION;
    settings->timezone_offset_minutes = s_timezone_offset;
    return ESP_OK;
}

time_t d1l_test_time(time_t *out_time)
{
    const time_t now = (time_t)(TEST_BUILD_EPOCH + 60);
    if (out_time) {
        *out_time = now;
    }
    return now;
}

#ifdef _WIN32
int settimeofday(const struct timeval *value, const void *timezone_value)
#else
int settimeofday(const struct timeval *value,
                 const struct timezone *timezone_value)
#endif
{
    (void)timezone_value;
    if (!value) {
        return -1;
    }
    return 0;
}

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    assert(config != NULL);
    s_sntp_wait_enabled = config->wait_for_sync;
    return s_sntp_init_result;
}

esp_err_t esp_netif_sntp_sync_wait(TickType_t ticks_to_wait)
{
    assert(ticks_to_wait > 0U);
    assert(s_sntp_wait_enabled);
    return s_sntp_wait_result;
}

static void assert_checkpoint_blob_unchanged(
    const uint8_t *expected, size_t expected_length)
{
    uint8_t actual[128] = {0};
    const size_t actual_length = mock_nvs_copy_blob(
        CHECKPOINT_NAMESPACE, CHECKPOINT_KEY, actual, sizeof(actual));
    assert(actual_length == expected_length);
    assert(memcmp(actual, expected, expected_length) == 0);
}

static void queue_newer_companion_during_save(void)
{
    assert(d1l_time_service_set_companion_time(
               TEST_BUILD_EPOCH +
                   3 * D1L_SETTINGS_TIME_CHECKPOINT_MIN_ADVANCE_SEC + 61,
               true) == ESP_OK);
}

static void run_happy_and_retry(void)
{
    mock_nvs_reset();
    mock_timer_set_us(0);
    assert(d1l_time_service_init() == ESP_OK);

    assert(d1l_time_service_wait_for_certificate_time(
               1000U, 100U, NULL, NULL) == ESP_OK);
    d1l_time_service_status_t status;
    d1l_time_service_status(&status);
    assert(status.wall_checkpoint_pending);
    assert(status.wall_checkpoint_write_count == 0U);
    assert(status.clock.wall_source == D1L_TIME_SOURCE_SNTP);
    assert(status.clock.protocol_wall_admission ==
           D1L_TIME_PROTOCOL_WALL_SNTP_ADMITTED);

    assert(d1l_time_service_wall_checkpoint_flush_if_due() == ESP_OK);
    d1l_time_service_status(&status);
    assert(!status.wall_checkpoint_pending);
    assert(status.wall_checkpoint_write_count == 1U);
    assert(status.wall_checkpoint.revision == 1U);
    assert(status.wall_checkpoint.protocol_reserved_through ==
           D1L_TIME_PROTOCOL_TIMESTAMP_BASE);

    assert(d1l_time_service_set_companion_time(
               TEST_BUILD_EPOCH + 61, true) == ESP_OK);
    assert(d1l_time_service_wall_checkpoint_flush_if_due() == ESP_OK);
    d1l_time_service_status(&status);
    assert(!status.wall_checkpoint_pending);
    assert(status.wall_checkpoint_write_count == 2U);
    assert(status.wall_checkpoint.revision == 2U);
    assert(status.wall_checkpoint.source ==
           D1L_SETTINGS_TIME_CHECKPOINT_SOURCE_COMPANION_AUTHENTICATED);

    assert(d1l_time_service_set_companion_time(
               TEST_BUILD_EPOCH + 62, true) == ESP_OK);
    assert(d1l_time_service_wall_checkpoint_flush_if_due() == ESP_OK);
    d1l_time_service_status(&status);
    assert(status.wall_checkpoint_skip_count == 1U);
    assert(status.wall_checkpoint.revision == 2U);

    assert(d1l_time_service_set_companion_time(
               TEST_BUILD_EPOCH +
                   D1L_SETTINGS_TIME_CHECKPOINT_MIN_ADVANCE_SEC + 61,
               true) == ESP_OK);
    mock_nvs_fail_next_set(ESP_FAIL);
    assert(d1l_time_service_wall_checkpoint_flush_if_due() == ESP_FAIL);
    d1l_time_service_status(&status);
    assert(status.wall_checkpoint_pending);
    assert(status.wall_checkpoint_failure_count == 1U);
    assert(status.wall_checkpoint_retry_not_before_us == UINT64_C(30000000));

    assert(d1l_time_service_wall_checkpoint_flush_if_due() == ESP_OK);
    d1l_time_service_status(&status);
    assert(status.wall_checkpoint_pending);
    assert(status.wall_checkpoint_failure_count == 1U);
    mock_timer_set_us(INT64_C(30000000));
    assert(d1l_time_service_wall_checkpoint_flush_if_due() == ESP_OK);
    d1l_time_service_status(&status);
    assert(!status.wall_checkpoint_pending);
    assert(status.wall_checkpoint_write_count == 3U);
    assert(status.wall_checkpoint.revision == 3U);

    assert(d1l_time_service_set_companion_time(
               TEST_BUILD_EPOCH +
                   2 * D1L_SETTINGS_TIME_CHECKPOINT_MIN_ADVANCE_SEC + 61,
               true) == ESP_OK);
    mock_nvs_run_during_next_set(queue_newer_companion_during_save);
    assert(d1l_time_service_wall_checkpoint_flush_if_due() == ESP_OK);
    d1l_time_service_status(&status);
    assert(status.wall_checkpoint_pending);
    assert(status.wall_checkpoint.revision == 4U);
    assert(d1l_time_service_wall_checkpoint_flush_if_due() == ESP_OK);
    d1l_time_service_status(&status);
    assert(!status.wall_checkpoint_pending);
    assert(status.wall_checkpoint.revision == 5U);
    assert(status.wall_checkpoint.epoch_sec ==
           TEST_BUILD_EPOCH +
               3 * D1L_SETTINGS_TIME_CHECKPOINT_MIN_ADVANCE_SEC + 61);
}

static void run_corrupt_isolation(void)
{
    mock_nvs_reset();
    uint8_t blob[sizeof(d1l_settings_envelope_header_t) + 32U] = {0};
    uint8_t payload[32] = {0};
    payload[0] = 1U;
    size_t blob_length = 0U;
    assert(d1l_settings_envelope_build(
        blob, sizeof(blob), payload, sizeof(payload), 1U, &blob_length));
    blob[sizeof(d1l_settings_envelope_header_t)] ^= 0x80U;
    assert(mock_nvs_seed_blob(
        CHECKPOINT_NAMESPACE, CHECKPOINT_KEY, blob, blob_length));
    uint8_t before[sizeof(blob)] = {0};
    const size_t before_length = mock_nvs_copy_blob(
        CHECKPOINT_NAMESPACE, CHECKPOINT_KEY, before, sizeof(before));

    assert(d1l_time_service_init() == ESP_OK);
    d1l_time_service_status_t status;
    d1l_time_service_status(&status);
    assert(status.protocol_persistence_ready);
    assert(status.wall_checkpoint.state ==
           D1L_SETTINGS_TIME_CHECKPOINT_QUARANTINED_CHECKSUM);
    assert(!status.wall_checkpoint_recovered);
    assert(status.wall_checkpoint_write_blocked);
    assert(!status.clock.wall_valid);

    assert(d1l_time_service_set_companion_time(
               TEST_BUILD_EPOCH + 60, true) == ESP_OK);
    assert(d1l_time_service_wall_checkpoint_flush_if_due() == ESP_OK);
    d1l_time_service_status(&status);
    assert(!status.wall_checkpoint_pending);
    assert(status.clock.certificate_time_valid);
    assert_checkpoint_blob_unchanged(before, before_length);
}

static void run_guard_rollback(void)
{
    mock_nvs_reset();
    const d1l_settings_time_checkpoint_t checkpoint = {
        .epoch_sec = TEST_BUILD_EPOCH + 100,
        .protocol_reserved_through =
            D1L_TIME_PROTOCOL_TIMESTAMP_BASE + 64U,
        .source = D1L_SETTINGS_TIME_CHECKPOINT_SOURCE_SNTP,
    };
    d1l_settings_time_checkpoint_status_t checkpoint_status;
    bool written = false;
    assert(d1l_settings_time_checkpoint_save(
               &checkpoint, &written, &checkpoint_status) == ESP_OK);
    assert(written);

    assert(d1l_time_service_init() == ESP_OK);
    d1l_time_service_status_t status;
    d1l_time_service_status(&status);
    assert(status.protocol_persistence_ready);
    assert(status.wall_checkpoint.state ==
           D1L_SETTINGS_TIME_CHECKPOINT_READY);
    assert(status.wall_checkpoint_recovery_error == ESP_ERR_INVALID_ARG);
    assert(!status.wall_checkpoint_recovered);
    assert(status.wall_checkpoint_write_blocked);
    assert(!status.clock.wall_valid);
}

static void run_legacy_migration(void)
{
    mock_nvs_reset();
    mock_timer_set_us(0);
    const uint32_t legacy = (uint32_t)TEST_BUILD_EPOCH - 100U;
    const uint32_t confirmed_upper = (uint32_t)TEST_BUILD_EPOCH + 200U;
    assert(mock_nvs_seed_blob(
        D1L_TIME_PROTOCOL_NVS_NAMESPACE, D1L_TIME_PROTOCOL_LEGACY_KEY,
        &legacy, sizeof(legacy)));

    assert(d1l_time_service_init() == ESP_ERR_INVALID_STATE);
    d1l_time_service_status_t status;
    d1l_time_service_status(&status);
    assert(status.protocol_migration.state ==
           D1L_TIME_PROTOCOL_MIGRATION_REQUIRED);
    assert(status.protocol_migration.observed_legacy_value == legacy);
    assert(!status.protocol_persistence_ready);
    assert(!status.protocol_tx_ready);
    assert(!status.clock.wall_valid);
    uint32_t timestamp = 0xA5A5A5A5U;
    assert(d1l_time_service_next_protocol_timestamp(&timestamp) ==
           ESP_ERR_INVALID_STATE);
    assert(timestamp == 0xA5A5A5A5U);

    bool written = true;
    d1l_time_protocol_migration_status_t migration = {0};
    assert(d1l_time_service_migrate_legacy_protocol_timestamp(
               legacy + 1U, confirmed_upper,
               D1L_TIME_PROTOCOL_MIGRATION_CONFIRMATION,
               &written, &migration) == ESP_ERR_INVALID_ARG);
    assert(!written);
    assert(migration.state == D1L_TIME_PROTOCOL_MIGRATION_REQUIRED);
    assert(d1l_time_service_migrate_legacy_protocol_timestamp(
               legacy, confirmed_upper,
               D1L_TIME_PROTOCOL_MIGRATION_CONFIRMATION,
               &written, &migration) == ESP_OK);
    assert(written);
    assert(migration.state == D1L_TIME_PROTOCOL_MIGRATION_COMPLETE);

    d1l_time_service_status(&status);
    assert(status.protocol_persistence_ready);
    assert(status.protocol_tx_ready);
    assert(status.protocol_migration.state ==
           D1L_TIME_PROTOCOL_MIGRATION_COMPLETE);
    assert(!status.clock.wall_valid);
    assert(d1l_time_service_next_protocol_timestamp(&timestamp) == ESP_OK);
    assert(timestamp == confirmed_upper + 1U);
    written = true;
    assert(d1l_time_service_migrate_legacy_protocol_timestamp(
               legacy, confirmed_upper,
               D1L_TIME_PROTOCOL_MIGRATION_CONFIRMATION,
               &written, &migration) == ESP_OK);
    assert(!written);
    assert(migration.state == D1L_TIME_PROTOCOL_MIGRATION_COMPLETE);
    written = true;
    assert(d1l_time_service_migrate_legacy_protocol_timestamp(
               legacy, confirmed_upper + 1U,
               D1L_TIME_PROTOCOL_MIGRATION_CONFIRMATION,
               &written, &migration) == ESP_ERR_INVALID_ARG);
    assert(!written);
    assert(migration.state == D1L_TIME_PROTOCOL_MIGRATION_COMPLETE);
    d1l_time_service_status(&status);
    assert(status.protocol_migration.observed_high_water >= timestamp);
    assert(!status.clock.wall_valid);
}

static void run_legacy_quarantine(void)
{
    mock_nvs_reset();
    const uint8_t malformed_receipt[] = {0x01U, 0x02U, 0x03U};
    assert(mock_nvs_seed_blob(
        D1L_TIME_PROTOCOL_MIGRATION_NVS_NAMESPACE,
        D1L_TIME_PROTOCOL_MIGRATION_NVS_KEY,
        malformed_receipt, sizeof(malformed_receipt)));
    assert(d1l_time_service_init() == ESP_ERR_INVALID_SIZE);
    d1l_time_service_status_t status;
    d1l_time_service_status(&status);
    assert(status.protocol_migration.state ==
           D1L_TIME_PROTOCOL_MIGRATION_QUARANTINED_MALFORMED);
    assert(status.protocol_migration.write_blocked);
    assert(status.protocol_persistence_state ==
           D1L_TIME_PROTOCOL_PERSISTENCE_CORRUPT);
    assert(!status.protocol_persistence_ready);
    assert(!status.protocol_tx_ready);
    uint32_t timestamp = 0x5A5A5A5AU;
    assert(d1l_time_service_next_protocol_timestamp(&timestamp) ==
           ESP_ERR_INVALID_SIZE);
    assert(timestamp == 0x5A5A5A5AU);
    assert(mock_nvs_set_call_count() == 0U);
    assert(mock_nvs_commit_call_count() == 0U);
}

static void run_legacy_preinit_failure(void)
{
    mock_nvs_reset();
    mock_semaphore_fail_next_create();
    bool written = true;
    d1l_time_protocol_migration_status_t status;
    memset(&status, 0xA5, sizeof(status));
    assert(d1l_time_service_migrate_legacy_protocol_timestamp(
               D1L_TIME_PROTOCOL_TIMESTAMP_BASE + 1U,
               D1L_TIME_PROTOCOL_TIMESTAMP_BASE + 65U,
               D1L_TIME_PROTOCOL_MIGRATION_CONFIRMATION,
               &written, &status) == ESP_ERR_NO_MEM);
    assert(!written);
    assert(status.state == D1L_TIME_PROTOCOL_MIGRATION_UNINITIALIZED);
    assert(status.error == ESP_ERR_NO_MEM);
    assert(!status.receipt_found);
    assert(!status.legacy_present);
    assert(!status.high_water_present);
    assert(!status.write_blocked);
    assert(mock_nvs_set_call_count() == 0U);
    assert(mock_nvs_commit_call_count() == 0U);
}

static void run_daylight_saving_display(void)
{
    mock_nvs_reset();
    mock_timer_set_us(0);
    assert(d1l_time_service_init() == ESP_OK);
    assert(d1l_time_service_set_companion_time(TEST_BUILD_EPOCH + 60, true) == ESP_OK);
    s_timezone_offset = -360;
    d1l_time_service_status_t before, after;
    d1l_time_service_status(&before);
    const unsigned writes = mock_nvs_set_call_count();
    s_daylight_saving = D1L_DAYLIGHT_SAVING_NORTH_AMERICA;
    d1l_time_service_status(&after);
    assert(after.timezone_standard_offset_minutes == -360);
    assert(after.timezone_offset_minutes == -300);
    assert(strcmp(after.timezone_label, "UTC-05:00") == 0);
    assert(after.display_time_valid);
    assert(strcmp(after.display_time, before.display_time) != 0);
    assert(after.clock.wall_epoch_sec == before.clock.wall_epoch_sec);
    assert(after.clock.protocol_trust_anchor == before.clock.protocol_trust_anchor);
    assert(mock_nvs_set_call_count() == writes);
    s_daylight_saving = D1L_DAYLIGHT_SAVING_OFF;
    d1l_time_service_status(&after);
    assert(after.timezone_offset_minutes == -360);
    assert(strcmp(after.display_time, before.display_time) == 0);
    s_timezone_offset = 60;
    s_daylight_saving = D1L_DAYLIGHT_SAVING_EUROPE;
    d1l_time_service_status(&after);
    assert(after.timezone_offset_minutes == 120);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "happy") == 0) {
        run_happy_and_retry();
    } else if (strcmp(argv[1], "corrupt") == 0) {
        run_corrupt_isolation();
    } else if (strcmp(argv[1], "guard") == 0) {
        run_guard_rollback();
    } else if (strcmp(argv[1], "legacy-migration") == 0) {
        run_legacy_migration();
    } else if (strcmp(argv[1], "legacy-quarantine") == 0) {
        run_legacy_quarantine();
    } else if (strcmp(argv[1], "legacy-preinit-failure") == 0) {
        run_legacy_preinit_failure();
    } else if (strcmp(argv[1], "daylight-saving") == 0) {
        run_daylight_saving_display();
    } else {
        assert(!"unknown scenario");
    }
    puts("native truthful-time checkpoint integration: ok");
    return 0;
}
void d1l_display_preferences_get(d1l_display_preferences_t *out) {
    *out = (d1l_display_preferences_t){.daylight_saving = s_daylight_saving};
}
