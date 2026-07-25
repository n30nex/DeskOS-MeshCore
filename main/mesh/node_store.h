#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define D1L_NODE_RAM_ACTIVE_CAPACITY 512U
#define D1L_NODE_NVS_FALLBACK_CAPACITY 16U
#define D1L_NODE_SD_HISTORY_CAPACITY 512U
#define D1L_NODE_STORE_CAPACITY D1L_NODE_SD_HISTORY_CAPACITY
#define D1L_NODE_STORE_PERSIST_MIN_INTERVAL_MS 5000U
#define D1L_NODE_STORE_PERSIST_MAX_INTERVAL_MS 30000U
#define D1L_NODE_FINGERPRINT_LEN 17U
#define D1L_NODE_PUBLIC_KEY_HEX_LEN 65U
#define D1L_HEARD_NODE_NAME_LEN 24U
#define D1L_NODE_TYPE_LEN 9U
#define D1L_NODE_ROLE_LEN 12U

typedef struct {
    uint32_t seq;
    uint32_t first_heard_ms;
    uint32_t last_heard_ms;
    uint32_t advert_timestamp;
    uint32_t heard_count;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    char name[D1L_HEARD_NODE_NAME_LEN];
    char type[D1L_NODE_TYPE_LEN];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool location_valid;
    int32_t lat_e6;
    int32_t lon_e6;
    uint32_t location_advert_timestamp;
    uint32_t location_seq;
} d1l_node_entry_t;

typedef enum {
    D1L_NODE_LOCATION_PROVENANCE_UNKNOWN = 0,
    D1L_NODE_LOCATION_PROVENANCE_SIGNED_ADVERT,
} d1l_node_location_provenance_t;

typedef struct {
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char name[D1L_HEARD_NODE_NAME_LEN];
    char type[D1L_NODE_TYPE_LEN];
    int32_t lat_e6;
    int32_t lon_e6;
    uint32_t location_advert_timestamp;
    uint32_t location_seq;
    d1l_node_location_provenance_t location_provenance;
} d1l_node_marker_t;

typedef enum {
    D1L_NODE_FILTER_ALL = 0,
    D1L_NODE_FILTER_COMPANION,
    D1L_NODE_FILTER_REPEATER,
    D1L_NODE_FILTER_ROOM,
    D1L_NODE_FILTER_SENSOR,
    D1L_NODE_FILTER_FAVORITE,
} d1l_node_filter_t;

typedef enum {
    D1L_NODE_SORT_LAST_HEARD = 0,
    D1L_NODE_SORT_SIGNAL,
    D1L_NODE_SORT_NAME,
    D1L_NODE_SORT_ROLE,
    D1L_NODE_SORT_FAVORITE,
} d1l_node_sort_t;

typedef struct {
    d1l_node_filter_t filter;
    d1l_node_sort_t sort;
    const char *text;
    bool keyed_only;
    bool reachable_only;
} d1l_node_query_t;

typedef struct {
    d1l_node_entry_t node;
    char display_name[D1L_HEARD_NODE_NAME_LEN];
    char role[D1L_NODE_ROLE_LEN];
    bool favorite;
    bool muted;
    bool keyed;
    bool reachable;
} d1l_node_view_t;

typedef struct {
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t persistence_commit_count;
    uint32_t persistence_coalesced_count;
    uint32_t persistence_fail_count;
    uint32_t sd_backend_generation;
    esp_err_t sd_primary_last_error;
    uint64_t persistence_revision;
    size_t count;
    size_t capacity;
    bool persistence_dirty;
    bool sd_primary_reconcile_pending;
} d1l_node_store_stats_t;

esp_err_t d1l_node_store_init(void);
esp_err_t d1l_node_store_clear(void);
esp_err_t d1l_node_store_flush(void);
esp_err_t d1l_node_store_flush_if_due(void);
/*
 * Accepts only a node's first or strictly newer signed advert timestamp.
 * Once a fingerprint has a full public key, a different full key sharing that
 * prefix is rejected with ESP_ERR_INVALID_STATE without changing retained data.
 * A newer locationless advert updates identity/heard metadata while retaining
 * the most recent valid location. out_stale reports an intentional replay
 * rejection separately from storage or initialization errors.
 */
esp_err_t d1l_node_store_upsert_advert(const char *fingerprint, const char *public_key_hex,
                                       const char *name, char type_code, int rssi_dbm,
                                       int snr_tenths, uint8_t path_hash_bytes,
                                       uint8_t path_hops, uint32_t advert_timestamp,
                                       bool location_valid, int32_t lat_e6, int32_t lon_e6,
                                       bool *out_stale);
d1l_node_store_stats_t d1l_node_store_stats(void);
bool d1l_node_store_find_by_fingerprint(const char *fingerprint, d1l_node_entry_t *out_entry);
size_t d1l_node_store_copy_recent(d1l_node_entry_t *out_entries, size_t max_entries);
size_t d1l_node_store_query(const d1l_node_query_t *query, d1l_node_view_t *out_entries,
                            size_t max_entries);
/* Changes only for marker membership, identity/name/type, or coordinate changes. */
uint32_t d1l_node_store_marker_generation(void);
/* Passive, bounded snapshot of located nodes, newest local location receipt first.
 * This store is written only by d1l_node_store_upsert_advert(), after the signed
 * advert gate. The transient marker therefore publishes that provenance
 * explicitly instead of asking the UI to infer it from coordinates. */
size_t d1l_node_store_copy_markers(d1l_node_marker_t *out_markers, size_t max_markers);
