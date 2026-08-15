#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "app/settings_protocol_migration.h"
#include "app/settings_time_checkpoint.h"
#include "platform/time_display.h"
#include "platform/time_service_core.h"

#define D1L_TIME_TLS_WAIT_TIMEOUT_MS 15000U
#define D1L_TIME_TLS_WAIT_SLICE_MS 500U

typedef bool (*d1l_time_continue_cb_t)(void *context);

typedef struct {
    d1l_time_core_snapshot_t clock;
    d1l_time_protocol_persistence_state_t protocol_persistence_state;
    d1l_time_protocol_migration_status_t protocol_migration;
    esp_err_t protocol_persistence_error;
    esp_err_t protocol_tx_error;
    esp_err_t sntp_init_error;
    d1l_settings_time_checkpoint_status_t wall_checkpoint;
    esp_err_t wall_checkpoint_recovery_error;
    uint32_t wall_checkpoint_write_count;
    uint32_t wall_checkpoint_skip_count;
    uint32_t wall_checkpoint_failure_count;
    uint64_t wall_checkpoint_retry_not_before_us;
    esp_err_t timezone_settings_error;
    uint16_t timezone_schema_version;
    int16_t timezone_offset_minutes;
    bool initialized;
    bool protocol_persistence_ready;
    bool protocol_tx_ready;
    bool sntp_initialized;
    bool wall_checkpoint_recovered;
    bool wall_checkpoint_pending;
    bool wall_checkpoint_write_blocked;
    bool timezone_settings_ready;
    bool display_time_valid;
    bool display_time_approximate;
    char timezone_label[D1L_TIMEZONE_LABEL_LEN];
    char display_time[D1L_TIME_DISPLAY_CLOCK_LEN];
} d1l_time_service_status_t;

esp_err_t d1l_time_service_init(void);
uint64_t d1l_time_service_boot_monotonic_us(void);
esp_err_t d1l_time_service_preflight_protocol_timestamp(void);
esp_err_t d1l_time_service_next_protocol_timestamp(uint32_t *out_timestamp);
esp_err_t d1l_time_service_migrate_legacy_protocol_timestamp(
    uint32_t expected_legacy_value,
    uint32_t confirmed_upper_bound,
    const char *confirmation,
    bool *out_written,
    d1l_time_protocol_migration_status_t *out_status);
void d1l_time_service_status(d1l_time_service_status_t *out_status);
bool d1l_time_service_certificate_time_valid(void);
esp_err_t d1l_time_service_wait_for_certificate_time(
    uint32_t timeout_ms,
    uint32_t slice_ms,
    d1l_time_continue_cb_t should_continue,
    void *continue_context);
esp_err_t d1l_time_service_wait_for_network_time(
    uint32_t timeout_ms,
    uint32_t slice_ms,
    d1l_time_continue_cb_t should_continue,
    void *continue_context);
/* `authenticated` is a capability boundary, not user input.  Only a verified
 * companion session owner may pass true; USB/CLI parsers must not expose it. */
esp_err_t d1l_time_service_set_companion_time(int64_t epoch_sec,
                                              bool authenticated);
esp_err_t d1l_time_service_note_authenticated_lower_bound(
    int64_t epoch_sec,
    bool authenticated);

/* The common retained worker is the only runtime writer. Forced flushes are
 * used by controlled-reboot quiescence; background passes honor retry
 * backoff. */
esp_err_t d1l_time_service_wall_checkpoint_flush(void);
esp_err_t d1l_time_service_wall_checkpoint_flush_if_due(void);

/* This service persists validated wall checkpoints without changing protocol
 * allocation semantics. An authenticated companion transport remains a WP-12
 * follow-up. Timezone conversion is fixed-offset presentation only;
 * protocol/certificate clocks remain UTC and no automatic DST is claimed. */
