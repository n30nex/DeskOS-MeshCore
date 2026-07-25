#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define D1L_ROUTE_STORE_CAPACITY 16U
#define D1L_ROUTE_STORE_NVS_FALLBACK_CAPACITY 4U
#define D1L_ROUTE_STORE_PERSIST_MIN_INTERVAL_MS 5000U
#define D1L_ROUTE_STORE_PERSIST_MAX_INTERVAL_MS 30000U
#define D1L_ROUTE_TARGET_LEN 17U
#define D1L_ROUTE_LABEL_LEN 24U
#define D1L_ROUTE_KIND_LEN 16U
#define D1L_ROUTE_NAME_LEN 18U

typedef struct {
    uint32_t seq;
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;
    uint32_t seen_count;
    char target[D1L_ROUTE_TARGET_LEN];
    char label[D1L_ROUTE_LABEL_LEN];
    char kind[D1L_ROUTE_KIND_LEN];
    char route[D1L_ROUTE_NAME_LEN];
    char direction[4];
    int last_rssi_dbm;
    int last_snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint8_t confidence;
    uint16_t payload_len;
} d1l_route_entry_t;

typedef struct {
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t persistence_commit_count;
    uint32_t persistence_coalesced_count;
    uint32_t persistence_fail_count;
    uint32_t persistence_stale_snapshot_count;
    uint32_t sd_primary_commit_count;
    uint32_t sd_primary_fail_count;
    uint32_t sd_backend_generation;
    esp_err_t sd_primary_last_error;
    uint32_t nvs_fallback_commit_count;
    uint32_t nvs_fallback_fail_count;
    esp_err_t nvs_fallback_last_error;
    uint32_t clear_fail_count;
    esp_err_t clear_last_error;
    uint64_t persistence_revision;
    size_t count;
    size_t capacity;
    bool persistence_dirty;
    bool sd_primary_required;
    bool sd_primary_dirty;
    bool sd_primary_reconcile_pending;
    bool nvs_fallback_dirty;
    bool clear_failure_latched;
} d1l_route_store_stats_t;

esp_err_t d1l_route_store_init(void);
esp_err_t d1l_route_store_clear(void);
esp_err_t d1l_route_store_clear_target(const char *target);
esp_err_t d1l_route_store_flush(void);
esp_err_t d1l_route_store_flush_if_due(void);
esp_err_t d1l_route_store_upsert_observation(const char *target, const char *label,
                                             const char *kind, const char *route,
                                             const char *direction, int rssi_dbm,
                                             int snr_tenths, uint8_t path_hash_bytes,
                                             uint8_t path_hops, uint16_t payload_len);
esp_err_t d1l_route_store_upsert_observation_volatile(const char *target, const char *label,
                                                      const char *kind, const char *route,
                                                      const char *direction, int rssi_dbm,
                                                      int snr_tenths, uint8_t path_hash_bytes,
                                                      uint8_t path_hops, uint16_t payload_len);
d1l_route_store_stats_t d1l_route_store_stats(void);
size_t d1l_route_store_copy_recent(d1l_route_entry_t *out_entries, size_t max_entries);
size_t d1l_route_store_copy_for_target(const char *target, d1l_route_entry_t *out_entries,
                                       size_t max_entries);
esp_err_t d1l_route_store_find_by_seq(uint32_t seq, d1l_route_entry_t *out_entry);
