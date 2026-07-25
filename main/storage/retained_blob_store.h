#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define D1L_RETAINED_BLOB_STORE_SD_DEGRADED_NOTE "SD degraded; using internal fallback"

typedef enum {
    D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES = 0,
    D1L_RETAINED_BLOB_STORE_DM_MESSAGES,
    D1L_RETAINED_BLOB_STORE_ROUTES,
    D1L_RETAINED_BLOB_STORE_PACKET_LOG,
    D1L_RETAINED_BLOB_STORE_COUNT,
} d1l_retained_blob_store_id_t;

typedef struct {
    uint32_t sd_read_fail_count;
    uint32_t sd_write_fail_count;
    uint32_t sd_rename_fail_count;
    esp_err_t sd_last_error;
    bool sd_degraded_latched;
    uint32_t nvs_mirror_fail_count;
    esp_err_t nvs_mirror_last_error;
} d1l_retained_blob_store_sd_stats_t;

typedef struct {
    bool enabled;
    uint32_t generation;
} d1l_retained_blob_store_backend_state_t;

/* Boot-local API telemetry. These counters measure retained-store NVS write
 * requests and successful commits, not physical flash program/erase cycles.
 * A successful unchanged-write suppression increments write_attempt_count but
 * neither write_commit_count nor write_fail_count. */
typedef struct {
    uint64_t write_attempt_count;
    uint64_t write_commit_count;
    uint64_t write_fail_count;
    uint64_t write_bytes_attempted;
    uint64_t write_bytes_committed;
    uint64_t erase_attempt_count;
    uint64_t erase_commit_count;
    uint64_t erase_fail_count;
    esp_err_t last_error;
} d1l_retained_blob_store_nvs_store_telemetry_t;

typedef struct {
    uint64_t write_attempt_count;
    uint64_t write_commit_count;
    uint64_t write_fail_count;
    uint64_t write_bytes_attempted;
    uint64_t write_bytes_committed;
    uint64_t erase_attempt_count;
    uint64_t erase_commit_count;
    uint64_t erase_fail_count;
    esp_err_t last_error;
    bool capacity_valid;
    esp_err_t capacity_error;
    size_t used_entries;
    size_t free_entries;
    size_t available_entries;
    size_t total_entries;
    size_t namespace_count;
    d1l_retained_blob_store_nvs_store_telemetry_t
        stores[D1L_RETAINED_BLOB_STORE_COUNT];
} d1l_retained_blob_store_nvs_telemetry_t;

esp_err_t d1l_retained_blob_store_init(void);
bool d1l_retained_blob_store_nvs_ready(void);
esp_err_t d1l_retained_blob_store_nvs_error(void);
bool d1l_retained_blob_store_nvs_marker_ready(void);
bool d1l_retained_blob_store_nvs_markers_complete(void);
bool d1l_retained_blob_store_nvs_anchor_ready(void);
bool d1l_retained_blob_store_nvs_sentinel_ready(void);
bool d1l_retained_blob_store_nvs_external_init_required(void);
bool d1l_retained_blob_store_nvs_initialized_this_boot(void);
uint32_t d1l_retained_blob_store_nvs_migrated_keys(void);
esp_err_t d1l_retained_blob_store_nvs_migration_error(void);
bool d1l_retained_blob_store_nvs_telemetry(
    d1l_retained_blob_store_nvs_telemetry_t *out_telemetry);
const char *d1l_retained_blob_store_backend_name(d1l_retained_blob_store_id_t store_id);
bool d1l_retained_blob_store_is_available(d1l_retained_blob_store_id_t store_id);
bool d1l_retained_blob_store_uses_sd(d1l_retained_blob_store_id_t store_id);
bool d1l_retained_blob_store_backend_state(
    d1l_retained_blob_store_id_t store_id,
    d1l_retained_blob_store_backend_state_t *out_state);
bool d1l_retained_blob_store_sd_stats(d1l_retained_blob_store_id_t store_id,
                                      d1l_retained_blob_store_sd_stats_t *out_stats);
bool d1l_retained_blob_store_any_sd_degraded(void);
void d1l_retained_blob_store_note_sd_backend(bool data_ready,
                                             bool file_ops_supported,
                                             bool atomic_rename_supported,
                                             uint32_t file_line_max,
                                             uint32_t file_chunk_max,
                                             uint32_t path_max);
esp_err_t d1l_retained_blob_store_read_sd_primary(
    d1l_retained_blob_store_id_t store_id,
    const char *key,
    void *dst,
    size_t *len_inout);
esp_err_t d1l_retained_blob_store_read_nvs_fallback(
    d1l_retained_blob_store_id_t store_id,
    const char *key,
    void *dst,
    size_t *len_inout);
esp_err_t d1l_retained_blob_store_write_sd_primary(
    d1l_retained_blob_store_id_t store_id,
    const char *key,
    const void *src,
    size_t len);
esp_err_t d1l_retained_blob_store_write_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id,
    const char *key,
    const void *src,
    size_t len,
    uint32_t expected_generation);
esp_err_t d1l_retained_blob_store_write_nvs_fallback(
    d1l_retained_blob_store_id_t store_id,
    const char *key,
    const void *src,
    size_t len);
esp_err_t d1l_retained_blob_store_erase_sd_primary(
    d1l_retained_blob_store_id_t store_id,
    const char *key);
esp_err_t d1l_retained_blob_store_erase_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id,
    const char *key,
    uint32_t expected_generation);
/* True only when the currently mounted backend generation carries the exact
 * durable marker for the onboard post-reset lineage and the onboard fence has
 * completed. Missing/mismatched markers are a normal false result. */
esp_err_t d1l_retained_blob_store_sd_media_lineage_ready(
    d1l_retained_blob_store_id_t store_id,
    uint32_t expected_backend_generation, bool *out_ready);
/* packet_log.c calls this only after deleting every exact segment path for the
 * same backend generation. It then erases the primary and clears the durable
 * reset lineage fence as one fail-closed completion sequence. */
esp_err_t d1l_retained_blob_store_complete_packet_reset_lineage(
    uint32_t expected_backend_generation);
esp_err_t d1l_retained_blob_store_erase_nvs_fallback(
    d1l_retained_blob_store_id_t store_id,
    const char *key);
esp_err_t d1l_retained_blob_store_read(d1l_retained_blob_store_id_t store_id,
                                       const char *key,
                                       void *dst,
                                       size_t *len_inout);
esp_err_t d1l_retained_blob_store_read_fallback(d1l_retained_blob_store_id_t store_id,
                                                const char *key,
                                                void *dst,
                                                size_t *len_inout);
esp_err_t d1l_retained_blob_store_write(d1l_retained_blob_store_id_t store_id,
                                        const char *key,
                                        const void *src,
                                        size_t len);
esp_err_t d1l_retained_blob_store_write_split(d1l_retained_blob_store_id_t store_id,
                                              const char *key,
                                              const void *primary_src,
                                              size_t primary_len,
                                              const void *fallback_src,
                                              size_t fallback_len);
esp_err_t d1l_retained_blob_store_erase(d1l_retained_blob_store_id_t store_id,
                                        const char *key);
