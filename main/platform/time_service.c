#include "time_service.h"

#include <stddef.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "app/settings_model.h"
#include "app/settings_protocol_migration.h"
#include "platform/time_display.h"

#ifndef D1L_BUILD_EPOCH_SEC
#error "D1L_BUILD_EPOCH_SEC must be derived from the exact source commit"
#endif

_Static_assert(D1L_BUILD_EPOCH_SEC >= D1L_TIME_PROTOCOL_TIMESTAMP_BASE,
               "build epoch predates the protocol epoch");
_Static_assert((int64_t)D1L_BUILD_EPOCH_SEC <=
                   D1L_TIME_SNTP_MAX_PROTOCOL_EPOCH,
               "build epoch leaves insufficient uint32 protocol headroom");

#define D1L_TIME_CHECKPOINT_RETRY_BACKOFF_US UINT64_C(30000000)

static StaticSemaphore_t s_time_mutex_storage;
static SemaphoreHandle_t s_time_mutex;
static d1l_time_service_core_t s_time_core;
static bool s_initialized;
static bool s_protocol_seed_ready;
static bool s_sntp_initialized;
static d1l_time_protocol_persistence_state_t s_protocol_persistence_state =
    D1L_TIME_PROTOCOL_PERSISTENCE_UNINITIALIZED;
static esp_err_t s_protocol_persistence_error = ESP_ERR_INVALID_STATE;
static d1l_time_protocol_migration_status_t s_protocol_migration_status = {
    .state = D1L_TIME_PROTOCOL_MIGRATION_UNINITIALIZED,
    .error = ESP_ERR_INVALID_STATE,
};
static esp_err_t s_sntp_init_error = ESP_ERR_INVALID_STATE;
static d1l_settings_time_checkpoint_status_t s_wall_checkpoint_status = {
    .state = D1L_SETTINGS_TIME_CHECKPOINT_UNINITIALIZED,
    .error = ESP_ERR_INVALID_STATE,
};
static esp_err_t s_wall_checkpoint_recovery_error = ESP_ERR_INVALID_STATE;
static uint32_t s_wall_checkpoint_write_count;
static uint32_t s_wall_checkpoint_skip_count;
static uint32_t s_wall_checkpoint_failure_count;
static bool s_wall_checkpoint_load_attempted;
static bool s_wall_checkpoint_recovered;
static d1l_settings_time_checkpoint_t s_wall_checkpoint_pending;
static uint64_t s_wall_checkpoint_pending_generation;
static uint64_t s_wall_checkpoint_retry_not_before_us;
static bool s_wall_checkpoint_dirty;
static bool s_wall_checkpoint_write_blocked;

static uint64_t monotonic_now_us(void)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)now_us : 0U;
}

static bool time_lock(void)
{
    return s_time_mutex &&
           xSemaphoreTake(s_time_mutex, portMAX_DELAY) == pdTRUE;
}

static void time_unlock(void)
{
    (void)xSemaphoreGive(s_time_mutex);
}

static esp_err_t load_protocol_seed_locked(void)
{
    /* mesh_ts is only a predecessor lower bound. Inspection must never infer
     * wall time from it or auto-promote it into mesh_hi_v2. Only an absent
     * legacy key or a completed exact-device receipt may seed protocol TX. */
    esp_err_t ret = d1l_time_protocol_migration_inspect(
        &s_protocol_migration_status);
    if (ret == ESP_OK) {
        const bool migration_allows_seed =
            s_protocol_migration_status.state ==
                D1L_TIME_PROTOCOL_MIGRATION_ABSENT ||
            s_protocol_migration_status.state ==
                D1L_TIME_PROTOCOL_MIGRATION_COMPLETE;
        if (!migration_allows_seed) {
            s_protocol_persistence_state =
                D1L_TIME_PROTOCOL_PERSISTENCE_MIGRATION_REQUIRED;
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    if (ret == ESP_OK) {
        const bool high_water_present =
            s_protocol_migration_status.high_water_present;
        const uint32_t reserved_through = high_water_present ?
            s_protocol_migration_status.observed_high_water :
            D1L_TIME_PROTOCOL_TIMESTAMP_BASE;
        s_protocol_persistence_state =
            d1l_time_core_classify_protocol_seed(
                false, high_water_present, reserved_through);
        if (s_protocol_persistence_state ==
            D1L_TIME_PROTOCOL_PERSISTENCE_FRESH) {
            ret = d1l_time_core_seed_protocol(
                &s_time_core, false, D1L_TIME_PROTOCOL_TIMESTAMP_BASE);
        } else if (s_protocol_persistence_state ==
                   D1L_TIME_PROTOCOL_PERSISTENCE_READY) {
            ret = d1l_time_core_seed_protocol(&s_time_core, true,
                                              reserved_through);
        } else {
            ret = ESP_ERR_INVALID_STATE;
        }
    } else {
        const bool migration_block =
            s_protocol_migration_status.state ==
                D1L_TIME_PROTOCOL_MIGRATION_REQUIRED ||
            s_protocol_migration_status.state ==
                D1L_TIME_PROTOCOL_MIGRATION_PENDING;
        const bool migration_quarantine =
            s_protocol_migration_status.state ==
                D1L_TIME_PROTOCOL_MIGRATION_QUARANTINED_NEWER_SCHEMA ||
            s_protocol_migration_status.state ==
                D1L_TIME_PROTOCOL_MIGRATION_QUARANTINED_MALFORMED ||
            s_protocol_migration_status.state ==
                D1L_TIME_PROTOCOL_MIGRATION_QUARANTINED_CHECKSUM ||
            s_protocol_migration_status.state ==
                D1L_TIME_PROTOCOL_MIGRATION_QUARANTINED_DOWNGRADE ||
            s_protocol_migration_status.state ==
                D1L_TIME_PROTOCOL_MIGRATION_REVISION_SATURATED;
        s_protocol_persistence_state = migration_block ?
            D1L_TIME_PROTOCOL_PERSISTENCE_MIGRATION_REQUIRED :
            (migration_quarantine ?
                D1L_TIME_PROTOCOL_PERSISTENCE_CORRUPT :
                D1L_TIME_PROTOCOL_PERSISTENCE_STORAGE_ERROR);
    }
    if (ret != ESP_OK &&
        s_protocol_persistence_state !=
            D1L_TIME_PROTOCOL_PERSISTENCE_MIGRATION_REQUIRED &&
        s_protocol_persistence_state !=
            D1L_TIME_PROTOCOL_PERSISTENCE_CORRUPT) {
        s_protocol_persistence_state =
            D1L_TIME_PROTOCOL_PERSISTENCE_STORAGE_ERROR;
    }
    s_protocol_seed_ready = ret == ESP_OK;
    s_protocol_persistence_error = ret;
    return ret;
}

static esp_err_t load_wall_checkpoint_locked(void)
{
    if (s_wall_checkpoint_load_attempted) {
        return s_wall_checkpoint_recovery_error;
    }
    s_wall_checkpoint_load_attempted = true;

    d1l_settings_time_checkpoint_t checkpoint = {0};
    d1l_settings_time_checkpoint_status_t status = {0};
    const esp_err_t load_ret = d1l_settings_time_checkpoint_load(
        &checkpoint, &status);
    s_wall_checkpoint_status = status;
    s_wall_checkpoint_recovery_error = load_ret;
    if (load_ret != ESP_OK || !status.found) {
        s_wall_checkpoint_write_blocked =
            status.state ==
                D1L_SETTINGS_TIME_CHECKPOINT_QUARANTINED_NEWER_SCHEMA ||
            status.state ==
                D1L_SETTINGS_TIME_CHECKPOINT_QUARANTINED_MALFORMED ||
            status.state ==
                D1L_SETTINGS_TIME_CHECKPOINT_QUARANTINED_CHECKSUM;
        return load_ret;
    }

    const esp_err_t recover_ret =
        d1l_time_core_recover_retained_checkpoint(
            &s_time_core, checkpoint.epoch_sec,
            checkpoint.protocol_reserved_through, monotonic_now_us());
    s_wall_checkpoint_recovery_error = recover_ret;
    s_wall_checkpoint_write_blocked = recover_ret != ESP_OK;
    s_wall_checkpoint_recovered =
        recover_ret == ESP_OK && s_time_core.wall_set &&
        s_time_core.wall_source ==
            D1L_TIME_SOURCE_RETAINED_VALIDATED_CHECKPOINT;
    return recover_ret;
}

static esp_err_t queue_validated_wall_checkpoint_locked(
    int64_t epoch_sec,
    d1l_settings_time_checkpoint_source_t source)
{
    if (s_wall_checkpoint_write_blocked) {
        return s_wall_checkpoint_status.error != ESP_OK ?
            s_wall_checkpoint_status.error : ESP_ERR_INVALID_STATE;
    }
    if (s_wall_checkpoint_pending_generation == UINT64_MAX) {
        s_wall_checkpoint_failure_count++;
        return ESP_ERR_INVALID_STATE;
    }
    s_wall_checkpoint_pending = (d1l_settings_time_checkpoint_t) {
        .epoch_sec = epoch_sec,
        .protocol_reserved_through =
            s_time_core.protocol_reserved_through,
        .source = source,
    };
    s_wall_checkpoint_pending_generation++;
    s_wall_checkpoint_dirty = true;
    return ESP_OK;
}

static esp_err_t flush_wall_checkpoint(bool force)
{
    if (!s_initialized) {
        const esp_err_t init_ret = d1l_time_service_init();
        if (init_ret != ESP_OK && !s_initialized) {
            return init_ret;
        }
    }
    if (!time_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_wall_checkpoint_dirty) {
        time_unlock();
        return ESP_OK;
    }
    const uint64_t now_us = monotonic_now_us();
    if (!force && now_us < s_wall_checkpoint_retry_not_before_us) {
        time_unlock();
        return ESP_OK;
    }
    const d1l_settings_time_checkpoint_t checkpoint =
        s_wall_checkpoint_pending;
    const uint64_t generation = s_wall_checkpoint_pending_generation;
    time_unlock();

    d1l_settings_time_checkpoint_status_t status = {0};
    bool written = false;
    const esp_err_t ret = d1l_settings_time_checkpoint_save(
        &checkpoint, &written, &status);

    /* NVS I/O above is deliberately outside the time mutex and the adoption
     * call path. A generation token prevents an in-flight save from clearing
     * a newer validated candidate. */
    if (!time_lock()) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    if (status.revision >= s_wall_checkpoint_status.revision ||
        s_wall_checkpoint_status.state ==
            D1L_SETTINGS_TIME_CHECKPOINT_UNINITIALIZED) {
        s_wall_checkpoint_status = status;
    }
    if (ret == ESP_OK) {
        if (written) {
            s_wall_checkpoint_write_count++;
        } else {
            s_wall_checkpoint_skip_count++;
        }
        s_wall_checkpoint_retry_not_before_us = 0U;
        if (generation == s_wall_checkpoint_pending_generation) {
            s_wall_checkpoint_dirty = false;
        }
    } else {
        s_wall_checkpoint_failure_count++;
        const bool permanent =
            status.state ==
                D1L_SETTINGS_TIME_CHECKPOINT_QUARANTINED_NEWER_SCHEMA ||
            status.state ==
                D1L_SETTINGS_TIME_CHECKPOINT_QUARANTINED_MALFORMED ||
            status.state ==
                D1L_SETTINGS_TIME_CHECKPOINT_QUARANTINED_CHECKSUM ||
            status.state ==
                D1L_SETTINGS_TIME_CHECKPOINT_REVISION_SATURATED;
        if (permanent) {
            s_wall_checkpoint_write_blocked = true;
            if (generation == s_wall_checkpoint_pending_generation) {
                s_wall_checkpoint_dirty = false;
            }
        } else {
            s_wall_checkpoint_retry_not_before_us =
                UINT64_MAX - now_us <
                        D1L_TIME_CHECKPOINT_RETRY_BACKOFF_US ?
                    UINT64_MAX :
                    now_us + D1L_TIME_CHECKPOINT_RETRY_BACKOFF_US;
        }
    }
    time_unlock();
    return ret;
}

static esp_err_t reserve_protocol_range(void *context,
                                        uint32_t reserved_through)
{
    (void)context;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_TIME_PROTOCOL_NVS_NAMESPACE,
                             NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        s_protocol_persistence_state =
            D1L_TIME_PROTOCOL_PERSISTENCE_STORAGE_ERROR;
        s_protocol_persistence_error = ret;
        return ret;
    }
    ret = nvs_set_u32(handle, D1L_TIME_PROTOCOL_HIGH_WATER_KEY,
                      reserved_through);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    s_protocol_persistence_state = ret == ESP_OK ?
        D1L_TIME_PROTOCOL_PERSISTENCE_READY :
        D1L_TIME_PROTOCOL_PERSISTENCE_STORAGE_ERROR;
    s_protocol_persistence_error = ret;
    if (ret == ESP_OK) {
        s_protocol_migration_status.high_water_present = true;
        s_protocol_migration_status.observed_high_water = reserved_through;
    }
    return ret;
}

static bool continue_allowed(d1l_time_continue_cb_t should_continue,
                             void *context)
{
    return !should_continue || should_continue(context);
}

static bool snapshot_time_ready_locked(bool require_validated_source)
{
    d1l_time_core_snapshot_t snapshot;
    d1l_time_core_snapshot(&s_time_core, monotonic_now_us(), &snapshot);
    const bool ready = snapshot.certificate_time_valid &&
                       snapshot.wall_epoch_sec >= D1L_TIME_WALL_MIN_EPOCH;
    return ready && (!require_validated_source ||
                     snapshot.wall_validity >=
                         D1L_TIME_VALIDITY_NETWORK_VALIDATED);
}

static esp_err_t accept_sntp_system_time(uint32_t expected_generation,
                                         bool require_validated_source)
{
    if (!time_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A companion or retained-source update that starts after this SNTP wait
     * was admitted supersedes the in-flight network adoption.  Sampling the
     * system clock and committing the source under this same lock gives the
     * transition one explicit linearization point. */
    if (s_time_core.wall_generation != expected_generation) {
        const bool ready =
            snapshot_time_ready_locked(require_validated_source);
        time_unlock();
        /* A validated companion source wins immediately.  A weaker
         * generation change only invalidates this admission token; let the
         * bounded caller refresh its token and retry instead of permanently
         * wedging certificate-time acquisition. */
        return ready ? ESP_OK : ESP_ERR_NOT_FINISHED;
    }
    const time_t now = time(NULL);
    if (now == (time_t)-1 || (int64_t)now < D1L_TIME_WALL_MIN_EPOCH) {
        time_unlock();
        return ESP_ERR_NOT_FINISHED;
    }
    const esp_err_t ret = d1l_time_core_set_wall_if_generation(
        &s_time_core, expected_generation, (int64_t)now,
        monotonic_now_us(), D1L_TIME_VALIDITY_NETWORK_VALIDATED,
        D1L_TIME_SOURCE_SNTP);
    const bool ready =
        ret == ESP_OK && snapshot_time_ready_locked(require_validated_source);
    const bool checkpoint_admitted =
        ready && d1l_time_core_wall_checkpoint_eligible(&s_time_core);
    if (checkpoint_admitted) {
        (void)queue_validated_wall_checkpoint_locked(
            (int64_t)now, D1L_SETTINGS_TIME_CHECKPOINT_SOURCE_SNTP);
    }
    time_unlock();
    return ready ? ESP_OK : (ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret);
}

static esp_err_t time_state(uint32_t *generation, bool *ready,
                            bool require_validated_source)
{
    if (!generation || !ready) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized && d1l_time_service_init() != ESP_OK && !s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!time_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_time_core_snapshot_t snapshot;
    d1l_time_core_snapshot(&s_time_core, monotonic_now_us(), &snapshot);
    *generation = snapshot.wall_generation;
    *ready = snapshot.certificate_time_valid &&
             snapshot.wall_epoch_sec >= D1L_TIME_WALL_MIN_EPOCH &&
             (!require_validated_source ||
              snapshot.wall_validity >= D1L_TIME_VALIDITY_NETWORK_VALIDATED);
    time_unlock();
    return ESP_OK;
}

static esp_err_t ensure_sntp_initialized(void)
{
    if (!time_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sntp_initialized) {
        time_unlock();
        return ESP_OK;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.wait_for_sync = true;
    const esp_err_t ret = esp_netif_sntp_init(&config);
    s_sntp_initialized = ret == ESP_OK;
    s_sntp_init_error = ret;
    time_unlock();
    return ret;
}

static void populate_timezone_status(d1l_time_service_status_t *status)
{
    (void)snprintf(status->display_time, sizeof(status->display_time),
                   "--:--");
    d1l_settings_t settings = {0};
    const esp_err_t ret = d1l_settings_public_snapshot(&settings);
    status->timezone_settings_error = ret;
    status->timezone_settings_ready =
        ret == ESP_OK && settings.timezone_schema_version ==
                             D1L_TIMEZONE_SETTING_SCHEMA_VERSION &&
        d1l_time_display_offset_valid(settings.timezone_offset_minutes);
    status->timezone_schema_version = status->timezone_settings_ready ?
        settings.timezone_schema_version :
        D1L_TIMEZONE_SETTING_SCHEMA_VERSION;
    status->timezone_offset_minutes = status->timezone_settings_ready ?
        settings.timezone_offset_minutes : 0;
    (void)d1l_time_display_timezone_label(
        status->timezone_offset_minutes, status->timezone_label,
        sizeof(status->timezone_label));
}

static void populate_display_time(d1l_time_service_status_t *status)
{
    status->display_time_valid = status->clock.wall_valid;
    status->display_time_approximate =
        status->clock.wall_validity == D1L_TIME_VALIDITY_APPROXIMATE;
    if (!status->display_time_valid || !d1l_time_display_format_clock(
            status->clock.wall_epoch_sec, status->timezone_offset_minutes,
            status->display_time_approximate, status->display_time,
            sizeof(status->display_time))) {
        status->display_time_valid = false;
        status->display_time_approximate = false;
        (void)snprintf(status->display_time, sizeof(status->display_time),
                       "--:--");
    }
}

esp_err_t d1l_time_service_init(void)
{
    if (!s_time_mutex) {
        s_time_mutex = xSemaphoreCreateMutexStatic(&s_time_mutex_storage);
        if (!s_time_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!time_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_initialized) {
        const esp_err_t core_ret = d1l_time_core_init(
            &s_time_core, monotonic_now_us(),
            (uint32_t)D1L_BUILD_EPOCH_SEC);
        if (core_ret != ESP_OK) {
            time_unlock();
            return core_ret;
        }
        s_initialized = true;
    }
    esp_err_t ret = ESP_OK;
    if (!s_protocol_seed_ready) {
        ret = load_protocol_seed_locked();
    }
    /* A damaged/future checkpoint remains quarantined and observable but is
     * never allowed to block the independent protocol high-water seed. Its
     * embedded guard is checked only after that seed is established. */
    if (s_protocol_seed_ready) {
        (void)load_wall_checkpoint_locked();
    }
    time_unlock();
    return ret;
}

uint64_t d1l_time_service_boot_monotonic_us(void)
{
    if (!s_initialized && d1l_time_service_init() != ESP_OK) {
        return 0U;
    }
    if (!time_lock()) {
        return 0U;
    }
    const uint64_t value =
        d1l_time_core_observe_monotonic(&s_time_core, monotonic_now_us());
    time_unlock();
    return value;
}

esp_err_t d1l_time_service_preflight_protocol_timestamp(void)
{
    esp_err_t ret = d1l_time_service_init();
    if (ret != ESP_OK && !s_protocol_seed_ready) {
        return ret;
    }
    if (!time_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_protocol_seed_ready) {
        ret = load_protocol_seed_locked();
    }
    if (ret == ESP_OK) {
        ret = d1l_time_core_preflight_protocol_timestamp(
            &s_time_core, monotonic_now_us());
    }
    time_unlock();
    return ret;
}

esp_err_t d1l_time_service_next_protocol_timestamp(uint32_t *out_timestamp)
{
    if (!out_timestamp) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = d1l_time_service_init();
    if (ret != ESP_OK && !s_protocol_seed_ready) {
        return ret;
    }
    if (!time_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_protocol_seed_ready) {
        ret = load_protocol_seed_locked();
    }
    if (ret == ESP_OK) {
        ret = d1l_time_core_next_protocol_timestamp(
            &s_time_core, monotonic_now_us(), reserve_protocol_range, NULL,
            out_timestamp);
    }
    time_unlock();
    return ret;
}

esp_err_t d1l_time_service_migrate_legacy_protocol_timestamp(
    uint32_t expected_legacy_value,
    uint32_t confirmed_upper_bound,
    const char *confirmation,
    bool *out_written,
    d1l_time_protocol_migration_status_t *out_status)
{
    if (out_written) {
        *out_written = false;
    }
    if (out_status) {
        *out_status = (d1l_time_protocol_migration_status_t) {
            .state = D1L_TIME_PROTOCOL_MIGRATION_UNINITIALIZED,
            .error = ESP_ERR_INVALID_STATE,
        };
    }
    if (!out_written || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Initialization is allowed to fail specifically because the legacy
     * lower bound has blocked protocol TX. The core still exists, but no
     * timestamp can have been issued through this service. */
    const esp_err_t init_ret = d1l_time_service_init();
    if (init_ret != ESP_OK && !s_initialized) {
        out_status->error = init_ret;
        return init_ret;
    }
    if (!time_lock()) {
        out_status->error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    if (s_time_core.protocol_started &&
        s_protocol_migration_status.state !=
            D1L_TIME_PROTOCOL_MIGRATION_COMPLETE) {
        *out_status = s_protocol_migration_status;
        time_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = d1l_time_protocol_migration_run(
        expected_legacy_value, confirmed_upper_bound, confirmation,
        out_written, out_status);
    s_protocol_migration_status = *out_status;
    if (ret == ESP_OK && !s_protocol_seed_ready) {
        ret = load_protocol_seed_locked();
        if (ret == ESP_OK) {
            (void)load_wall_checkpoint_locked();
        }
        *out_status = s_protocol_migration_status;
    }
    time_unlock();
    return ret;
}

void d1l_time_service_status(d1l_time_service_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    *out_status = (d1l_time_service_status_t) {0};
    populate_timezone_status(out_status);
    out_status->protocol_persistence_state = s_protocol_persistence_state;
    out_status->protocol_migration = s_protocol_migration_status;
    out_status->protocol_persistence_error = s_protocol_persistence_error;
    out_status->wall_checkpoint = s_wall_checkpoint_status;
    out_status->wall_checkpoint_recovery_error =
        s_wall_checkpoint_recovery_error;
    if (!s_initialized && d1l_time_service_init() != ESP_OK && !s_initialized) {
        out_status->protocol_persistence_state =
            s_protocol_persistence_state;
        out_status->protocol_migration = s_protocol_migration_status;
        out_status->protocol_persistence_error = s_protocol_persistence_error;
        out_status->protocol_tx_error = ESP_ERR_INVALID_STATE;
        out_status->sntp_init_error = s_sntp_init_error;
        out_status->wall_checkpoint_write_count =
            s_wall_checkpoint_write_count;
        out_status->wall_checkpoint_skip_count =
            s_wall_checkpoint_skip_count;
        out_status->wall_checkpoint_failure_count =
            s_wall_checkpoint_failure_count;
        out_status->wall_checkpoint_recovered =
            s_wall_checkpoint_recovered;
        out_status->wall_checkpoint_retry_not_before_us =
            s_wall_checkpoint_retry_not_before_us;
        out_status->wall_checkpoint_pending = s_wall_checkpoint_dirty;
        out_status->wall_checkpoint_write_blocked =
            s_wall_checkpoint_write_blocked;
        return;
    }
    if (!time_lock()) {
        out_status->protocol_persistence_error = ESP_ERR_INVALID_STATE;
        out_status->protocol_tx_error = ESP_ERR_INVALID_STATE;
        out_status->sntp_init_error = s_sntp_init_error;
        out_status->wall_checkpoint_write_count =
            s_wall_checkpoint_write_count;
        out_status->wall_checkpoint_skip_count =
            s_wall_checkpoint_skip_count;
        out_status->wall_checkpoint_failure_count =
            s_wall_checkpoint_failure_count;
        out_status->wall_checkpoint_recovered =
            s_wall_checkpoint_recovered;
        out_status->wall_checkpoint_retry_not_before_us =
            s_wall_checkpoint_retry_not_before_us;
        out_status->wall_checkpoint_pending = s_wall_checkpoint_dirty;
        out_status->wall_checkpoint_write_blocked =
            s_wall_checkpoint_write_blocked;
        return;
    }
    d1l_time_core_snapshot(&s_time_core, monotonic_now_us(),
                           &out_status->clock);
    out_status->initialized = s_initialized;
    out_status->protocol_persistence_state = s_protocol_persistence_state;
    out_status->protocol_migration = s_protocol_migration_status;
    out_status->protocol_persistence_ready =
        s_protocol_seed_ready && s_protocol_persistence_error == ESP_OK;
    out_status->protocol_persistence_error = s_protocol_persistence_error;
    out_status->protocol_tx_ready =
        out_status->protocol_persistence_ready &&
        out_status->clock.protocol_tx_ready;
    out_status->protocol_tx_error =
        out_status->protocol_persistence_ready ?
            out_status->clock.protocol_tx_error :
            s_protocol_persistence_error;
    out_status->sntp_initialized = s_sntp_initialized;
    out_status->sntp_init_error = s_sntp_init_error;
    out_status->wall_checkpoint = s_wall_checkpoint_status;
    out_status->wall_checkpoint_recovery_error =
        s_wall_checkpoint_recovery_error;
    out_status->wall_checkpoint_write_count =
        s_wall_checkpoint_write_count;
    out_status->wall_checkpoint_skip_count =
        s_wall_checkpoint_skip_count;
    out_status->wall_checkpoint_failure_count =
        s_wall_checkpoint_failure_count;
    out_status->wall_checkpoint_recovered = s_wall_checkpoint_recovered;
    out_status->wall_checkpoint_retry_not_before_us =
        s_wall_checkpoint_retry_not_before_us;
    out_status->wall_checkpoint_pending = s_wall_checkpoint_dirty;
    out_status->wall_checkpoint_write_blocked =
        s_wall_checkpoint_write_blocked;
    populate_display_time(out_status);
    time_unlock();
}

bool d1l_time_service_certificate_time_valid(void)
{
    uint32_t generation = 0U;
    bool ready = false;
    return time_state(&generation, &ready, false) == ESP_OK && ready;
}

static esp_err_t wait_for_time(
    uint32_t timeout_ms,
    uint32_t slice_ms,
    d1l_time_continue_cb_t should_continue,
    void *continue_context,
    bool require_validated_source)
{
    if (slice_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!continue_allowed(should_continue, continue_context)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t expected_generation = 0U;
    bool certificate_ready = false;
    esp_err_t ret = time_state(&expected_generation, &certificate_ready,
                               require_validated_source);
    if (ret != ESP_OK) {
        return ret;
    }
    if (certificate_ready) {
        return ESP_OK;
    }
    if (timeout_ms == 0U) {
        return ESP_ERR_TIMEOUT;
    }
    ret = ensure_sntp_initialized();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = time_state(&expected_generation, &certificate_ready,
                     require_validated_source);
    if (ret != ESP_OK) {
        return ret;
    }
    if (certificate_ready) {
        return ESP_OK;
    }

    uint32_t elapsed_ms = 0U;
    while (elapsed_ms < timeout_ms) {
        if (!continue_allowed(should_continue, continue_context)) {
            return ESP_ERR_INVALID_STATE;
        }
        uint32_t wait_ms = timeout_ms - elapsed_ms;
        if (wait_ms > slice_ms) {
            wait_ms = slice_ms;
        }
        TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
        if (wait_ticks == 0U) {
            wait_ticks = 1U;
        }
        ret = esp_netif_sntp_sync_wait(wait_ticks);
        if (!continue_allowed(should_continue, continue_context)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (ret == ESP_OK) {
            const esp_err_t accept_ret =
                accept_sntp_system_time(expected_generation,
                                        require_validated_source);
            if (accept_ret == ESP_OK) {
                return ESP_OK;
            }
            if (accept_ret == ESP_ERR_NOT_FINISHED) {
                const esp_err_t state_ret = time_state(
                    &expected_generation, &certificate_ready,
                    require_validated_source);
                if (state_ret != ESP_OK) {
                    return state_ret;
                }
                if (certificate_ready) {
                    return ESP_OK;
                }
            } else {
                return accept_ret;
            }
        } else {
            uint32_t observed_generation = 0U;
            bool observed_ready = false;
            const esp_err_t state_ret =
                time_state(&observed_generation, &observed_ready,
                           require_validated_source);
            if (state_ret != ESP_OK) {
                return state_ret;
            }
            if (observed_ready) {
                return ESP_OK;
            }
        }
        if (ret != ESP_ERR_TIMEOUT && ret != ESP_ERR_NOT_FINISHED &&
            ret != ESP_OK) {
            return ret;
        }
        elapsed_ms += wait_ms;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t d1l_time_service_wait_for_certificate_time(
    uint32_t timeout_ms,
    uint32_t slice_ms,
    d1l_time_continue_cb_t should_continue,
    void *continue_context)
{
    return wait_for_time(timeout_ms, slice_ms, should_continue,
                         continue_context, false);
}

esp_err_t d1l_time_service_wait_for_network_time(
    uint32_t timeout_ms,
    uint32_t slice_ms,
    d1l_time_continue_cb_t should_continue,
    void *continue_context)
{
    return wait_for_time(timeout_ms, slice_ms, should_continue,
                         continue_context, true);
}

esp_err_t d1l_time_service_set_companion_time(int64_t epoch_sec,
                                              bool authenticated)
{
    if (!authenticated || epoch_sec < D1L_TIME_WALL_MIN_EPOCH) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized && d1l_time_service_init() != ESP_OK && !s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!time_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    struct timeval value = {
        .tv_sec = (time_t)epoch_sec,
        .tv_usec = 0,
    };
    const esp_err_t preflight = d1l_time_core_preflight_wall(
        &s_time_core, epoch_sec,
        D1L_TIME_VALIDITY_COMPANION_VALIDATED,
        D1L_TIME_SOURCE_COMPANION_AUTHENTICATED);
    if ((int64_t)value.tv_sec != epoch_sec || preflight != ESP_OK) {
        time_unlock();
        return preflight == ESP_OK ? ESP_ERR_INVALID_ARG : preflight;
    }
    if (settimeofday(&value, NULL) != 0) {
        time_unlock();
        return ESP_FAIL;
    }
    /* The preflight and commit share this mutex, so no generation or source
     * invariant can change after the system-clock side effect. */
    const esp_err_t ret = d1l_time_core_set_wall(
        &s_time_core, epoch_sec, monotonic_now_us(),
        D1L_TIME_VALIDITY_COMPANION_VALIDATED,
        D1L_TIME_SOURCE_COMPANION_AUTHENTICATED);
    if (ret == ESP_OK &&
        d1l_time_core_wall_checkpoint_eligible(&s_time_core)) {
        (void)queue_validated_wall_checkpoint_locked(
            epoch_sec,
            D1L_SETTINGS_TIME_CHECKPOINT_SOURCE_COMPANION_AUTHENTICATED);
    }
    time_unlock();
    return ret;
}

esp_err_t d1l_time_service_note_authenticated_lower_bound(
    int64_t epoch_sec,
    bool authenticated)
{
    if (!authenticated || epoch_sec < D1L_TIME_WALL_MIN_EPOCH) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized && d1l_time_service_init() != ESP_OK && !s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!time_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = d1l_time_core_note_authenticated_lower_bound(
        &s_time_core, epoch_sec, monotonic_now_us());
    time_unlock();
    return ret;
}

esp_err_t d1l_time_service_wall_checkpoint_flush(void)
{
    return flush_wall_checkpoint(true);
}

esp_err_t d1l_time_service_wall_checkpoint_flush_if_due(void)
{
    return flush_wall_checkpoint(false);
}

/* Compatibility boundary for the current MeshCore command owner.  The
 * allocator itself is solely owned by this service; callers migrate without
 * editing the active protocol hotspot in this slice. */
esp_err_t d1l_settings_next_mesh_timestamp(uint32_t *timestamp)
{
    return d1l_time_service_next_protocol_timestamp(timestamp);
}
