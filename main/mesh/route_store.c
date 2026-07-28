#include "route_store.h"

#include <stdbool.h>
#include <string.h>

#include "esp_timer.h"
#include "nvs.h"

#include "mesh/store_lock.h"
#include "storage/retained_blob_store.h"

#define D1L_ROUTE_STORE_ID D1L_RETAINED_BLOB_STORE_ROUTES
#define D1L_ROUTE_STORE_LEGACY_KEY "routes"
#define D1L_ROUTE_STORE_V2_KEY "routes_v2"
#define D1L_ROUTE_STORE_LEGACY_SCHEMA 1U
#define D1L_ROUTE_STORE_FULL_SCHEMA 2U
#define D1L_ROUTE_STORE_FALLBACK_SCHEMA 3U
#define D1L_ROUTE_STORE_MAX_PAYLOAD_LEN 255U

typedef struct {
    uint32_t schema;
    uint32_t epoch;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_route_entry_t entries[D1L_ROUTE_STORE_CAPACITY];
} d1l_route_store_blob_t;

typedef struct {
    uint32_t schema;
    uint32_t epoch;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_route_entry_t entries[D1L_ROUTE_STORE_NVS_FALLBACK_CAPACITY];
} d1l_route_store_fallback_blob_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_route_entry_t entries[D1L_ROUTE_STORE_CAPACITY];
} d1l_route_store_legacy_blob_t;

typedef struct {
    uint64_t revision;
    bool sd_attempt;
    bool nvs_attempt;
    d1l_route_store_blob_t primary;
    d1l_route_store_fallback_blob_t fallback;
} d1l_route_persist_snapshot_t;

typedef enum {
    D1L_ROUTE_SOURCE_NONE,
    D1L_ROUTE_SOURCE_SD_V2,
    D1L_ROUTE_SOURCE_NVS_V2,
    D1L_ROUTE_SOURCE_SD_LEGACY,
    D1L_ROUTE_SOURCE_NVS_LEGACY,
} d1l_route_source_t;

static d1l_route_entry_t s_entries[D1L_ROUTE_STORE_CAPACITY];
static d1l_route_entry_t s_volatile_entry;
static size_t s_count;
static uint32_t s_next_seq = 1;
static uint32_t s_total_written;
static uint32_t s_dropped_oldest;
static uint32_t s_epoch = 1U;
static bool s_loaded;
static bool s_volatile_valid;
static d1l_route_store_blob_t s_blob_scratch;
static d1l_route_store_fallback_blob_t s_fallback_blob_scratch;
static d1l_route_store_legacy_blob_t s_legacy_sd_blob_scratch;
static d1l_route_store_legacy_blob_t s_legacy_nvs_blob_scratch;
static d1l_route_persist_snapshot_t s_persist_snapshot;
static d1l_route_entry_t s_reconcile_overlay[D1L_ROUTE_STORE_CAPACITY];
static bool s_persistence_dirty;
static bool s_persistence_material_dirty;
static bool s_sd_primary_dirty;
static bool s_sd_reconcile_pending;
static bool s_nvs_fallback_dirty;
static bool s_last_sd_primary_required;
static uint32_t s_last_sd_backend_generation;
static bool s_persisted_this_boot;
static bool s_persist_attempted_this_boot;
static bool s_dirty_timing_started;
static bool s_persistence_immediate_due;
static bool s_mutated_since_init;
static bool s_retry_pending;
static uint32_t s_boot_next_seq;
static uint32_t s_mutations_since_init;
static uint32_t s_dirty_since_ms;
static uint32_t s_material_dirty_since_ms;
static uint32_t s_last_persist_ms;
static uint32_t s_last_persist_attempt_ms;
static uint32_t s_persistence_commit_count;
static uint32_t s_persistence_coalesced_count;
static uint32_t s_persistence_fail_count;
static uint32_t s_persistence_stale_snapshot_count;
static uint32_t s_sd_primary_commit_count;
static uint32_t s_sd_primary_fail_count;
static esp_err_t s_sd_primary_last_error;
static uint32_t s_nvs_fallback_commit_count;
static uint32_t s_nvs_fallback_fail_count;
static esp_err_t s_nvs_fallback_last_error;
static uint32_t s_clear_fail_count;
static esp_err_t s_clear_last_error;
static bool s_clear_failure_latched;
static bool s_nvs_legacy_reclaim_allowed;
static uint64_t s_revision;
static d1l_store_lock_t s_store_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_persist_io_lock = D1L_STORE_LOCK_INITIALIZER;

static void load_valid_blob_into_ram(void);
static esp_err_t ensure_route_store_initialized(void);
static esp_err_t note_sd_reconcile_failure(esp_err_t failure, uint32_t now_ms);
static esp_err_t reconcile_sd_primary(uint32_t now_ms,
                                      uint32_t expected_backend_generation);

static bool sd_backend_generation_matches(uint32_t expected_generation)
{
    d1l_retained_blob_store_backend_state_t state = {0};
    return d1l_retained_blob_store_backend_state(D1L_ROUTE_STORE_ID, &state) &&
           state.enabled && state.generation == expected_generation;
}

static void sanitize_ascii(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }
    size_t out = 0;
    while (src && src[0] && out + 1U < dest_size) {
        unsigned char c = (unsigned char)*src++;
        if (c < 32 || c > 126 || c == '"' || c == '\\') {
            c = '_';
        }
        dest[out++] = (char)c;
    }
    dest[out] = '\0';
}

static uint8_t route_confidence(uint8_t path_hops)
{
    if (path_hops == 0) {
        return 100;
    }
    if (path_hops >= 4) {
        return 40;
    }
    return (uint8_t)(100U - (path_hops * 20U));
}

static void clear_ram(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    memset(&s_volatile_entry, 0, sizeof(s_volatile_entry));
    s_volatile_valid = false;
    s_count = 0;
    s_next_seq = 1;
    s_total_written = 0;
    s_dropped_oldest = 0;
}

static void reset_persistence_state(bool sd_primary_required,
                                    uint32_t sd_backend_generation)
{
    s_persistence_dirty = false;
    s_persistence_material_dirty = false;
    s_sd_primary_dirty = false;
    s_sd_reconcile_pending = false;
    s_nvs_fallback_dirty = false;
    s_last_sd_primary_required = sd_primary_required;
    s_last_sd_backend_generation = sd_backend_generation;
    s_persisted_this_boot = false;
    s_persist_attempted_this_boot = false;
    s_dirty_timing_started = false;
    s_persistence_immediate_due = false;
    s_mutated_since_init = false;
    s_retry_pending = false;
    s_boot_next_seq = 1U;
    s_mutations_since_init = 0U;
    s_dirty_since_ms = 0;
    s_material_dirty_since_ms = 0;
    s_last_persist_ms = 0;
    s_last_persist_attempt_ms = 0;
    s_persistence_commit_count = 0;
    s_persistence_coalesced_count = 0;
    s_persistence_fail_count = 0;
    s_persistence_stale_snapshot_count = 0;
    s_sd_primary_commit_count = 0;
    s_sd_primary_fail_count = 0;
    s_sd_primary_last_error = ESP_OK;
    s_nvs_fallback_commit_count = 0;
    s_nvs_fallback_fail_count = 0;
    s_nvs_fallback_last_error = ESP_OK;
    s_clear_fail_count = 0;
    s_clear_last_error = ESP_OK;
    s_clear_failure_latched = false;
    s_nvs_legacy_reclaim_allowed = false;
    s_revision = 0;
    s_epoch = 1U;
}

static void fill_blob(d1l_route_store_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->schema = D1L_ROUTE_STORE_FULL_SCHEMA;
    blob->epoch = s_epoch;
    blob->next_seq = s_next_seq;
    blob->total_written = s_total_written;
    blob->dropped_oldest = s_dropped_oldest;
    for (size_t i = 0; i < s_count; ++i) {
        if (blob->count < D1L_ROUTE_STORE_CAPACITY) {
            blob->entries[blob->count++] = s_entries[i];
        }
    }
}

static void fill_fallback_blob(d1l_route_store_fallback_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->schema = D1L_ROUTE_STORE_FALLBACK_SCHEMA;
    blob->epoch = s_epoch;
    blob->next_seq = s_next_seq;
    blob->total_written = s_total_written;
    blob->dropped_oldest = s_dropped_oldest;

    bool used[D1L_ROUTE_STORE_CAPACITY] = {0};
    const size_t wanted = s_count < D1L_ROUTE_STORE_NVS_FALLBACK_CAPACITY ?
                          s_count : D1L_ROUTE_STORE_NVS_FALLBACK_CAPACITY;
    while (blob->count < wanted) {
        size_t best = 0;
        bool best_set = false;
        for (size_t i = 0; i < s_count; ++i) {
            if (!used[i] &&
                (!best_set || s_entries[i].seq > s_entries[best].seq)) {
                best = i;
                best_set = true;
            }
        }
        if (!best_set) {
            break;
        }
        used[best] = true;
        blob->entries[blob->count++] = s_entries[best];
    }
}

static bool persistence_dirty_locked(bool sd_primary_required)
{
    return s_clear_failure_latched || s_nvs_fallback_dirty ||
           (sd_primary_required &&
            (s_sd_primary_dirty || s_sd_reconcile_pending));
}

static void note_persistence_dirty_locked(bool material, bool immediate,
                                          uint32_t now_ms)
{
    if (!s_dirty_timing_started) {
        s_dirty_timing_started = true;
        s_dirty_since_ms = now_ms;
    }
    if (material && !s_persistence_material_dirty) {
        s_material_dirty_since_ms = now_ms;
    }
    s_persistence_material_dirty = s_persistence_material_dirty || material;
    s_persistence_immediate_due = s_persistence_immediate_due || immediate;
    s_persistence_dirty = true;
}

static void clear_persistence_timing_locked(void)
{
    s_dirty_timing_started = false;
    s_persistence_immediate_due = false;
    s_dirty_since_ms = 0;
    s_material_dirty_since_ms = 0;
}

/* A legacy route blob can consume enough NVS entries that the compact v2 key
 * cannot be allocated.  Route history is a rebuildable cache, so after the
 * normal v2 write proves that capacity is the only blocker, reclaim only the
 * known legacy route key and retry.  No settings, identity, or other namespace
 * is touched.  A failed erase/retry remains dirty and blocks controlled reboot. */
static esp_err_t write_nvs_fallback_with_legacy_reclaim(
    const d1l_route_store_fallback_blob_t *fallback)
{
    esp_err_t ret = d1l_retained_blob_store_write_nvs_fallback(
        D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_V2_KEY,
        fallback, sizeof(*fallback));
    if (ret != ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    const bool reclaim_allowed = s_nvs_legacy_reclaim_allowed;
    d1l_store_lock_give(&s_store_lock);
    if (!reclaim_allowed) {
        return ret;
    }

    const esp_err_t erase_ret = d1l_retained_blob_store_erase_nvs_fallback(
        D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_LEGACY_KEY);
    if (erase_ret != ESP_OK) {
        return erase_ret;
    }
    d1l_store_lock_take(&s_store_lock);
    s_nvs_legacy_reclaim_allowed = false;
    d1l_store_lock_give(&s_store_lock);

    return d1l_retained_blob_store_write_nvs_fallback(
        D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_V2_KEY,
        fallback, sizeof(*fallback));
}

static esp_err_t persist_route_snapshot(bool force)
{
    d1l_store_lock_take(&s_persist_io_lock);
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    if (!d1l_retained_blob_store_backend_state(D1L_ROUTE_STORE_ID,
                                               &backend_state)) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool sd_primary_required = backend_state.enabled;
    const uint32_t sd_backend_generation = backend_state.generation;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    d1l_store_lock_take(&s_store_lock);
    if (!s_loaded) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (sd_backend_generation != s_last_sd_backend_generation) {
        s_last_sd_backend_generation = sd_backend_generation;
        s_sd_reconcile_pending = true;
        if (sd_primary_required) {
            note_persistence_dirty_locked(true, true, now_ms);
        }
    }
    s_last_sd_primary_required = sd_primary_required;
    s_persistence_dirty = persistence_dirty_locked(sd_primary_required);
    if (s_clear_failure_latched) {
        const esp_err_t ret = s_clear_last_error == ESP_OK ?
                              ESP_ERR_INVALID_STATE : s_clear_last_error;
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }
    if (!s_persistence_dirty) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }

    const uint32_t elapsed_since_attempt = now_ms - s_last_persist_attempt_ms;
    const bool retry_ready = !s_persist_attempted_this_boot ||
        elapsed_since_attempt >= D1L_ROUTE_STORE_PERSIST_MIN_INTERVAL_MS;
    const bool material_due = s_dirty_timing_started &&
        s_persistence_material_dirty &&
        now_ms - s_material_dirty_since_ms >= D1L_ROUTE_STORE_PERSIST_MIN_INTERVAL_MS;
    const bool deadline_due = s_dirty_timing_started &&
        now_ms - s_dirty_since_ms >= D1L_ROUTE_STORE_PERSIST_MAX_INTERVAL_MS;
    const bool recovery_due = s_retry_pending && retry_ready;
    const bool immediate_reconcile = sd_primary_required &&
        s_sd_reconcile_pending && s_persistence_immediate_due && !s_retry_pending;
    if (force && s_retry_pending && !retry_ready) {
        esp_err_t pending_error = ESP_ERR_INVALID_STATE;
        if ((s_sd_primary_dirty || s_sd_reconcile_pending) &&
            s_sd_primary_last_error != ESP_OK) {
            pending_error = s_sd_primary_last_error;
        } else if (s_nvs_fallback_dirty &&
                   s_nvs_fallback_last_error != ESP_OK) {
            pending_error = s_nvs_fallback_last_error;
        }
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return pending_error;
    }
    if (!force &&
        ((!retry_ready && !immediate_reconcile) ||
         (!s_persistence_immediate_due && !material_due &&
          !deadline_due && !recovery_due))) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }

    const bool reconcile_attempt = sd_primary_required && s_sd_reconcile_pending;
    s_persist_attempted_this_boot = true;
    s_last_persist_attempt_ms = now_ms;
    s_persistence_immediate_due = false;
    d1l_store_lock_give(&s_store_lock);

    if (reconcile_attempt) {
        const esp_err_t reconcile_ret = reconcile_sd_primary(
            now_ms, sd_backend_generation);
        if (reconcile_ret != ESP_OK) {
            if (reconcile_ret == ESP_ERR_NOT_FINISHED) {
                d1l_store_lock_give(&s_persist_io_lock);
                return reconcile_ret;
            }
            /* SD remains fail-closed, but a dirty compact fallback is an
             * independent durability target. Snapshot it after the failed
             * read so live observations still advance NVS without permitting
             * any SD write or a controlled reboot. */
            d1l_store_lock_take(&s_store_lock);
            const bool nvs_attempt = s_nvs_fallback_dirty;
            if (nvs_attempt) {
                fill_fallback_blob(&s_persist_snapshot.fallback);
                s_persist_snapshot.revision = s_revision;
            }
            d1l_store_lock_give(&s_store_lock);

            esp_err_t nvs_ret = ESP_OK;
            if (nvs_attempt) {
                nvs_ret = write_nvs_fallback_with_legacy_reclaim(
                    &s_persist_snapshot.fallback);
                d1l_store_lock_take(&s_store_lock);
                const bool same_revision =
                    s_revision == s_persist_snapshot.revision;
                s_nvs_fallback_last_error = nvs_ret;
                if (nvs_ret == ESP_OK) {
                    s_nvs_fallback_commit_count++;
                    if (same_revision) {
                        s_nvs_fallback_dirty = false;
                    }
                } else {
                    s_nvs_fallback_fail_count++;
                    s_persistence_fail_count++;
                }
                if (!same_revision) {
                    s_persistence_stale_snapshot_count++;
                }
                s_persistence_dirty = persistence_dirty_locked(sd_primary_required);
                s_retry_pending = true;
                d1l_store_lock_give(&s_store_lock);
            }
            d1l_store_lock_give(&s_persist_io_lock);
            return reconcile_ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    s_persistence_dirty = persistence_dirty_locked(sd_primary_required);
    if (!s_persistence_dirty) {
        s_persistence_material_dirty = false;
        clear_persistence_timing_locked();
        if (!s_sd_reconcile_pending) {
            s_mutated_since_init = false;
            s_mutations_since_init = 0U;
            s_boot_next_seq = s_next_seq;
        }
        s_retry_pending = false;
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }
    fill_blob(&s_persist_snapshot.primary);
    fill_fallback_blob(&s_persist_snapshot.fallback);
    s_persist_snapshot.revision = s_revision;
    s_persist_snapshot.sd_attempt = sd_primary_required && s_sd_primary_dirty;
    s_persist_snapshot.nvs_attempt = s_nvs_fallback_dirty;
    d1l_store_lock_give(&s_store_lock);

    esp_err_t sd_ret = ESP_OK;
    esp_err_t nvs_ret = ESP_OK;
    if (s_persist_snapshot.sd_attempt) {
        if (!sd_backend_generation_matches(sd_backend_generation)) {
            const esp_err_t generation_ret = note_sd_reconcile_failure(
                ESP_ERR_INVALID_STATE, now_ms);
            d1l_store_lock_give(&s_persist_io_lock);
            return generation_ret;
        }
        sd_ret = d1l_retained_blob_store_write_sd_primary_guarded(
            D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_V2_KEY,
            &s_persist_snapshot.primary, sizeof(s_persist_snapshot.primary),
            sd_backend_generation);
        if (sd_ret == ESP_ERR_NOT_FINISHED) {
            d1l_store_lock_give(&s_persist_io_lock);
            return sd_ret;
        }
    }
    if (s_persist_snapshot.nvs_attempt) {
        nvs_ret = write_nvs_fallback_with_legacy_reclaim(
            &s_persist_snapshot.fallback);
    }
    const bool sd_generation_changed = s_persist_snapshot.sd_attempt &&
        !sd_backend_generation_matches(sd_backend_generation);
    if (sd_generation_changed) {
        sd_ret = ESP_ERR_INVALID_STATE;
    }

    d1l_store_lock_take(&s_store_lock);
    const bool same_revision = s_revision == s_persist_snapshot.revision;
    if (s_persist_snapshot.sd_attempt) {
        if (sd_generation_changed) {
            s_sd_reconcile_pending = true;
        }
        s_sd_primary_last_error = sd_ret;
        if (sd_ret == ESP_OK) {
            s_sd_primary_commit_count++;
            if (same_revision) {
                s_sd_primary_dirty = false;
            }
        } else {
            s_sd_primary_fail_count++;
        }
    }
    if (s_persist_snapshot.nvs_attempt) {
        s_nvs_fallback_last_error = nvs_ret;
        if (nvs_ret == ESP_OK) {
            s_nvs_fallback_commit_count++;
            if (same_revision) {
                s_nvs_fallback_dirty = false;
            }
        } else {
            s_nvs_fallback_fail_count++;
        }
    }
    if (!same_revision) {
        s_persistence_stale_snapshot_count++;
    }
    const bool operation_failed = sd_ret != ESP_OK || nvs_ret != ESP_OK;
    if (operation_failed) {
        s_persistence_fail_count++;
    }
    s_persistence_dirty = persistence_dirty_locked(sd_primary_required);
    if (s_persistence_dirty && (operation_failed || !same_revision)) {
        s_retry_pending = true;
    }
    if (!s_persistence_dirty) {
        s_persistence_material_dirty = false;
        clear_persistence_timing_locked();
        s_persisted_this_boot = true;
        if (!s_sd_reconcile_pending) {
            s_mutated_since_init = false;
            s_mutations_since_init = 0U;
            s_boot_next_seq = s_next_seq;
        }
        s_retry_pending = false;
        s_last_persist_ms = now_ms;
        s_persistence_commit_count++;
    }
    const bool still_dirty = s_persistence_dirty;
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);

    if (sd_ret != ESP_OK) {
        return sd_ret;
    }
    if (nvs_ret != ESP_OK) {
        return nvs_ret;
    }
    return force && still_dirty ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static bool persisted_text_is_valid(const char *text, size_t capacity,
                                    bool require_nonempty)
{
    if (!text || capacity == 0U) {
        return false;
    }
    const char *terminator = memchr(text, '\0', capacity);
    if (!terminator || (require_nonempty && terminator == text)) {
        return false;
    }
    for (const char *cursor = text; cursor < terminator; ++cursor) {
        const unsigned char c = (unsigned char)*cursor;
        if (c < 32U || c > 126U || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

static bool persisted_route_entry_is_valid(const d1l_route_entry_t *entry,
                                            uint32_t next_seq)
{
    if (!entry || entry->seq == 0U || entry->seq >= next_seq ||
        !persisted_text_is_valid(entry->target, sizeof(entry->target), true) ||
        !persisted_text_is_valid(entry->label, sizeof(entry->label), true) ||
        !persisted_text_is_valid(entry->kind, sizeof(entry->kind), true) ||
        !persisted_text_is_valid(entry->route, sizeof(entry->route), true) ||
        !persisted_text_is_valid(entry->direction, sizeof(entry->direction), true) ||
        entry->seen_count == 0U || entry->confidence > 100U ||
        entry->path_hash_bytes == 0U || entry->path_hash_bytes > 3U ||
        entry->path_hops > 63U ||
        (uint16_t)entry->path_hash_bytes * entry->path_hops > 64U ||
        entry->payload_len > D1L_ROUTE_STORE_MAX_PAYLOAD_LEN) {
        return false;
    }
    return true;
}

static bool persisted_route_entries_are_valid(const d1l_route_entry_t *entries,
                                               size_t count,
                                               uint32_t next_seq,
                                               uint32_t total_written)
{
    if (!entries || total_written < count) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!persisted_route_entry_is_valid(&entries[i], next_seq)) {
            return false;
        }
        for (size_t j = i + 1U; j < count; ++j) {
            if (entries[i].seq == entries[j].seq) {
                return false;
            }
        }
    }
    return true;
}

static bool blob_is_valid(const d1l_route_store_blob_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_ROUTE_STORE_FULL_SCHEMA &&
           blob->epoch > 0U &&
           blob->count <= D1L_ROUTE_STORE_CAPACITY &&
           blob->next_seq > 0U &&
           persisted_route_entries_are_valid(blob->entries, blob->count,
                                              blob->next_seq,
                                              blob->total_written);
}

static bool fallback_blob_is_valid(const d1l_route_store_fallback_blob_t *blob,
                                   size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_ROUTE_STORE_FALLBACK_SCHEMA &&
           blob->epoch > 0U &&
           blob->count <= D1L_ROUTE_STORE_NVS_FALLBACK_CAPACITY &&
           blob->next_seq > 0U &&
           persisted_route_entries_are_valid(blob->entries, blob->count,
                                              blob->next_seq,
                                              blob->total_written);
}

static bool legacy_blob_is_valid(const d1l_route_store_legacy_blob_t *blob,
                                 size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_ROUTE_STORE_LEGACY_SCHEMA &&
           blob->count <= D1L_ROUTE_STORE_CAPACITY &&
           blob->next_seq > 0U &&
           persisted_route_entries_are_valid(blob->entries, blob->count,
                                              blob->next_seq,
                                              blob->total_written);
}

static void load_fallback_blob_into_ram(const d1l_route_store_fallback_blob_t *blob)
{
    clear_ram();
    memcpy(s_entries, blob->entries, blob->count * sizeof(blob->entries[0]));
    s_count = blob->count;
    s_next_seq = blob->next_seq;
    s_total_written = blob->total_written;
    s_dropped_oldest = blob->dropped_oldest;
    s_epoch = blob->epoch;
    s_persisted_this_boot = true;
}

static void load_valid_blob_into_ram(void)
{
    memcpy(s_entries, s_blob_scratch.entries, sizeof(s_entries));
    s_count = s_blob_scratch.count;
    s_next_seq = s_blob_scratch.next_seq;
    s_total_written = s_blob_scratch.total_written;
    s_dropped_oldest = s_blob_scratch.dropped_oldest;
    s_epoch = s_blob_scratch.epoch;
    s_persisted_this_boot = true;
}

static void load_legacy_blob_into_ram(const d1l_route_store_legacy_blob_t *blob)
{
    clear_ram();
    memcpy(s_entries, blob->entries, sizeof(s_entries));
    s_count = blob->count;
    s_next_seq = blob->next_seq;
    s_total_written = blob->total_written;
    s_dropped_oldest = blob->dropped_oldest;
    s_epoch = 1U;
    s_persisted_this_boot = true;
}

static int find_index(const char *target, const char *kind, const char *direction)
{
    if (!target || target[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < s_count; ++i) {
        if (strncmp(s_entries[i].target, target, sizeof(s_entries[i].target)) == 0 &&
            strncmp(s_entries[i].kind, kind ? kind : "", sizeof(s_entries[i].kind)) == 0 &&
            strncmp(s_entries[i].direction, direction ? direction : "", sizeof(s_entries[i].direction)) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static size_t oldest_index(void)
{
    size_t oldest = 0;
    for (size_t i = 1; i < s_count; ++i) {
        if (s_entries[i].seq < s_entries[oldest].seq) {
            oldest = i;
        }
    }
    return oldest;
}

static void legacy_blob_to_full(const d1l_route_store_legacy_blob_t *legacy,
                                d1l_route_store_blob_t *full)
{
    memset(full, 0, sizeof(*full));
    full->schema = D1L_ROUTE_STORE_FULL_SCHEMA;
    full->epoch = 1U;
    full->next_seq = legacy->next_seq;
    full->total_written = legacy->total_written;
    full->dropped_oldest = legacy->dropped_oldest;
    full->count = legacy->count;
    memcpy(full->entries, legacy->entries,
           legacy->count * sizeof(legacy->entries[0]));
}

static bool route_identity_matches(const d1l_route_entry_t *left,
                                   const d1l_route_entry_t *right)
{
    return left && right &&
           strncmp(left->target, right->target, sizeof(left->target)) == 0 &&
           strncmp(left->kind, right->kind, sizeof(left->kind)) == 0 &&
           strncmp(left->direction, right->direction, sizeof(left->direction)) == 0;
}

static bool merge_primary_entries_locked(const d1l_route_store_blob_t *primary)
{
    bool changed = false;
    for (size_t source = 0; source < primary->count; ++source) {
        const d1l_route_entry_t *candidate = &primary->entries[source];
        size_t match = 0;
        bool match_set = false;
        for (size_t current = 0; current < s_count; ++current) {
            if (route_identity_matches(&s_entries[current], candidate)) {
                match = current;
                match_set = true;
                break;
            }
        }
        if (match_set) {
            if (candidate->seq > s_entries[match].seq) {
                s_entries[match] = *candidate;
                changed = true;
            }
            continue;
        }
        if (s_count < D1L_ROUTE_STORE_CAPACITY) {
            s_entries[s_count++] = *candidate;
            changed = true;
            continue;
        }
        const size_t oldest = oldest_index();
        if (candidate->seq > s_entries[oldest].seq) {
            s_entries[oldest] = *candidate;
            changed = true;
        }
    }
    return changed;
}

static void adopt_primary_with_live_overlay_locked(void)
{
    size_t overlay_count = 0;
    for (size_t i = 0; i < s_count && overlay_count < D1L_ROUTE_STORE_CAPACITY; ++i) {
        if (s_entries[i].seq >= s_boot_next_seq) {
            s_reconcile_overlay[overlay_count++] = s_entries[i];
        }
    }
    for (size_t i = 1; i < overlay_count; ++i) {
        const d1l_route_entry_t ordered = s_reconcile_overlay[i];
        size_t position = i;
        while (position > 0U &&
               s_reconcile_overlay[position - 1U].seq > ordered.seq) {
            s_reconcile_overlay[position] = s_reconcile_overlay[position - 1U];
            position--;
        }
        s_reconcile_overlay[position] = ordered;
    }
    const uint32_t overlay_mutations = s_mutations_since_init;

    load_valid_blob_into_ram();
    for (size_t i = 0; i < overlay_count; ++i) {
        const d1l_route_entry_t overlay = s_reconcile_overlay[i];
        int existing = find_index(overlay.target, overlay.kind, overlay.direction);
        size_t index;
        if (existing >= 0) {
            index = (size_t)existing;
        } else if (s_count < D1L_ROUTE_STORE_CAPACITY) {
            index = s_count++;
        } else {
            index = oldest_index();
            s_dropped_oldest++;
        }
        s_entries[index] = overlay;
        s_entries[index].seq = s_next_seq++;
    }
    s_total_written += overlay_mutations;
}

static esp_err_t note_sd_reconcile_failure(esp_err_t failure, uint32_t now_ms)
{
    d1l_store_lock_take(&s_store_lock);
    s_sd_reconcile_pending = true;
    s_sd_primary_last_error = failure;
    s_sd_primary_fail_count++;
    s_persistence_fail_count++;
    s_retry_pending = true;
    note_persistence_dirty_locked(true, true, now_ms);
    s_persistence_dirty = true;
    d1l_store_lock_give(&s_store_lock);
    return failure;
}

/* Called only while the persistence I/O lock is held. A compact NVS fallback
 * is not proof that an unreadable or late-mounted SD primary is obsolete. Read
 * and compare the primary before permitting any SD write. */
static esp_err_t reconcile_sd_primary(uint32_t now_ms,
                                      uint32_t expected_backend_generation)
{
    if (!sd_backend_generation_matches(expected_backend_generation)) {
        return note_sd_reconcile_failure(ESP_ERR_INVALID_STATE, now_ms);
    }
    size_t full_len = sizeof(s_blob_scratch);
    esp_err_t full_ret = d1l_retained_blob_store_read_sd_primary(
        D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_V2_KEY,
        &s_blob_scratch, &full_len);
    if (full_ret == ESP_ERR_NOT_FINISHED) {
        return full_ret;
    }
    bool primary_is_v2 = false;
    bool primary_found = false;

    if (full_ret == ESP_OK) {
        if (!blob_is_valid(&s_blob_scratch, full_len)) {
            return note_sd_reconcile_failure(ESP_ERR_INVALID_STATE, now_ms);
        }
        primary_is_v2 = true;
        primary_found = true;
        if (s_blob_scratch.epoch == 1U) {
            size_t legacy_len = sizeof(s_legacy_sd_blob_scratch);
            const esp_err_t legacy_ret = d1l_retained_blob_store_read_sd_primary(
                D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_LEGACY_KEY,
                &s_legacy_sd_blob_scratch, &legacy_len);
            if (legacy_ret == ESP_ERR_NOT_FINISHED) {
                return legacy_ret;
            }
            if (legacy_ret == ESP_OK) {
                if (!legacy_blob_is_valid(&s_legacy_sd_blob_scratch, legacy_len)) {
                    return note_sd_reconcile_failure(ESP_ERR_INVALID_STATE, now_ms);
                }
                if (s_legacy_sd_blob_scratch.total_written >
                        s_blob_scratch.total_written ||
                    (s_legacy_sd_blob_scratch.total_written ==
                         s_blob_scratch.total_written &&
                     s_legacy_sd_blob_scratch.count > s_blob_scratch.count)) {
                    legacy_blob_to_full(&s_legacy_sd_blob_scratch, &s_blob_scratch);
                    primary_is_v2 = false;
                }
            } else if (legacy_ret != ESP_ERR_NOT_FOUND) {
                return note_sd_reconcile_failure(legacy_ret, now_ms);
            }
        }
    } else if (full_ret == ESP_ERR_NOT_FOUND) {
        size_t legacy_len = sizeof(s_legacy_sd_blob_scratch);
        const esp_err_t legacy_ret = d1l_retained_blob_store_read_sd_primary(
            D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_LEGACY_KEY,
            &s_legacy_sd_blob_scratch, &legacy_len);
        if (legacy_ret == ESP_ERR_NOT_FINISHED) {
            return legacy_ret;
        }
        if (legacy_ret == ESP_OK) {
            if (!legacy_blob_is_valid(&s_legacy_sd_blob_scratch, legacy_len)) {
                return note_sd_reconcile_failure(ESP_ERR_INVALID_STATE, now_ms);
            }
            legacy_blob_to_full(&s_legacy_sd_blob_scratch, &s_blob_scratch);
            primary_found = true;
        } else if (legacy_ret != ESP_ERR_NOT_FOUND) {
            return note_sd_reconcile_failure(legacy_ret, now_ms);
        }
    } else {
        return note_sd_reconcile_failure(full_ret, now_ms);
    }

    /* Never merge a snapshot assembled across two mount generations. A fast
     * remove/reinsert can finish between worker polls while the final enabled
     * level remains true; the generation makes that edge observable. */
    if (!sd_backend_generation_matches(expected_backend_generation)) {
        return note_sd_reconcile_failure(ESP_ERR_INVALID_STATE, now_ms);
    }

    d1l_store_lock_take(&s_store_lock);
    if (!primary_found) {
        s_sd_reconcile_pending = false;
        s_sd_primary_last_error = ESP_OK;
        s_sd_primary_dirty = s_total_written > 0U || s_epoch > 1U;
        if (s_sd_primary_dirty) {
            note_persistence_dirty_locked(true, true, now_ms);
        }
        s_persistence_dirty = persistence_dirty_locked(true);
        d1l_store_lock_give(&s_store_lock);
        return ESP_OK;
    }

    const uint32_t current_epoch = s_epoch;
    const uint32_t current_total = s_total_written;
    const bool primary_newer =
        s_blob_scratch.epoch > current_epoch ||
        (s_blob_scratch.epoch == current_epoch &&
         s_blob_scratch.total_written > current_total);
    const bool primary_same_generation =
        s_blob_scratch.epoch == current_epoch &&
        s_blob_scratch.total_written == current_total;

    if (primary_newer && s_mutated_since_init) {
        adopt_primary_with_live_overlay_locked();
        s_revision++;
        s_sd_primary_dirty = true;
        s_nvs_fallback_dirty = true;
    } else if (primary_same_generation && s_mutated_since_init) {
        if (merge_primary_entries_locked(&s_blob_scratch)) {
            s_revision++;
            s_nvs_fallback_dirty = true;
        }
        s_sd_primary_dirty = true;
    } else if (primary_newer || primary_same_generation) {
        const bool metadata_changed =
            s_blob_scratch.epoch != current_epoch ||
            s_blob_scratch.total_written != current_total;
        load_valid_blob_into_ram();
        s_revision++;
        s_mutated_since_init = false;
        s_mutations_since_init = 0U;
        s_boot_next_seq = s_next_seq;
        s_sd_primary_dirty = !primary_is_v2;
        s_nvs_fallback_dirty = s_nvs_fallback_dirty || metadata_changed;
    } else if (s_blob_scratch.epoch == current_epoch) {
        if (merge_primary_entries_locked(&s_blob_scratch)) {
            s_revision++;
            /* Refill an under-capacity compact fallback with the newest
             * members of the reconciled union, not just the SD primary. */
            s_nvs_fallback_dirty = true;
        }
        s_sd_primary_dirty = true;
    } else {
        /* The compact fallback carries a newer clear/data epoch. Never merge
         * older SD rows across that boundary; replace only after comparison. */
        s_sd_primary_dirty = true;
    }

    s_sd_reconcile_pending = false;
    s_sd_primary_last_error = ESP_OK;
    if (s_sd_primary_dirty || s_nvs_fallback_dirty) {
        note_persistence_dirty_locked(true, true, now_ms);
    }
    s_persistence_dirty = persistence_dirty_locked(true);
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

static esp_err_t route_store_init_internal(bool force_reload)
{
    d1l_store_lock_take(&s_persist_io_lock);
    d1l_store_lock_take(&s_store_lock);
    if (!force_reload && s_loaded) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    if (!d1l_retained_blob_store_backend_state(D1L_ROUTE_STORE_ID,
                                               &backend_state)) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool sd_primary_required = backend_state.enabled;
    clear_ram();
    reset_persistence_state(sd_primary_required, backend_state.generation);
    s_loaded = false;
    d1l_store_lock_give(&s_store_lock);

    size_t compact_len = sizeof(s_fallback_blob_scratch);
    const esp_err_t compact_ret = d1l_retained_blob_store_read_nvs_fallback(
        D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_V2_KEY,
        &s_fallback_blob_scratch, &compact_len);
    const bool compact_valid = compact_ret == ESP_OK &&
        fallback_blob_is_valid(&s_fallback_blob_scratch, compact_len);
    if ((compact_ret == ESP_OK && !compact_valid) ||
        (compact_ret != ESP_OK && compact_ret != ESP_ERR_NOT_FOUND)) {
        const esp_err_t failure = compact_ret == ESP_OK ?
                                  ESP_ERR_INVALID_STATE : compact_ret;
        d1l_store_lock_take(&s_store_lock);
        s_nvs_fallback_dirty = true;
        s_nvs_fallback_fail_count++;
        s_nvs_fallback_last_error = failure;
        s_persistence_fail_count++;
        s_persistence_dirty = true;
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return failure;
    }

    size_t sd_len = sizeof(s_blob_scratch);
    const esp_err_t sd_ret = sd_primary_required ?
        d1l_retained_blob_store_read_sd_primary(
            D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_V2_KEY,
            &s_blob_scratch, &sd_len) : ESP_ERR_NOT_FOUND;
    const bool sd_valid = sd_ret == ESP_OK && blob_is_valid(&s_blob_scratch, sd_len);

    size_t legacy_nvs_len = sizeof(s_legacy_nvs_blob_scratch);
    const esp_err_t legacy_nvs_ret = d1l_retained_blob_store_read_nvs_fallback(
        D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_LEGACY_KEY,
        &s_legacy_nvs_blob_scratch, &legacy_nvs_len);
    const bool legacy_nvs_valid = legacy_nvs_ret == ESP_OK &&
        legacy_blob_is_valid(&s_legacy_nvs_blob_scratch, legacy_nvs_len);

    size_t legacy_sd_len = sizeof(s_legacy_sd_blob_scratch);
    const esp_err_t legacy_sd_ret = sd_primary_required ?
        d1l_retained_blob_store_read_sd_primary(
            D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_LEGACY_KEY,
            &s_legacy_sd_blob_scratch, &legacy_sd_len) : ESP_ERR_NOT_FOUND;
    const bool legacy_sd_valid = legacy_sd_ret == ESP_OK &&
        legacy_blob_is_valid(&s_legacy_sd_blob_scratch, legacy_sd_len);

    if (sd_primary_required &&
        !sd_backend_generation_matches(backend_state.generation)) {
        const esp_err_t failure = note_sd_reconcile_failure(
            ESP_ERR_INVALID_STATE,
            (uint32_t)(esp_timer_get_time() / 1000ULL));
        d1l_store_lock_give(&s_persist_io_lock);
        return failure;
    }

    d1l_route_source_t v2_source = D1L_ROUTE_SOURCE_NONE;
    if (sd_valid && compact_valid) {
        const bool compact_newer =
            s_fallback_blob_scratch.epoch > s_blob_scratch.epoch ||
            (s_fallback_blob_scratch.epoch == s_blob_scratch.epoch &&
             s_fallback_blob_scratch.total_written > s_blob_scratch.total_written);
        v2_source = compact_newer ? D1L_ROUTE_SOURCE_NVS_V2 :
                                   D1L_ROUTE_SOURCE_SD_V2;
    } else if (sd_valid) {
        v2_source = D1L_ROUTE_SOURCE_SD_V2;
    } else if (compact_valid) {
        v2_source = D1L_ROUTE_SOURCE_NVS_V2;
    }

    d1l_route_source_t legacy_source = D1L_ROUTE_SOURCE_NONE;
    if (legacy_sd_valid && legacy_nvs_valid) {
        legacy_source = s_legacy_nvs_blob_scratch.total_written >
                        s_legacy_sd_blob_scratch.total_written ?
                        D1L_ROUTE_SOURCE_NVS_LEGACY : D1L_ROUTE_SOURCE_SD_LEGACY;
    } else if (legacy_sd_valid) {
        legacy_source = D1L_ROUTE_SOURCE_SD_LEGACY;
    } else if (legacy_nvs_valid) {
        legacy_source = D1L_ROUTE_SOURCE_NVS_LEGACY;
    }

    d1l_route_source_t source = v2_source != D1L_ROUTE_SOURCE_NONE ?
                                v2_source : legacy_source;
    if (v2_source != D1L_ROUTE_SOURCE_NONE &&
        legacy_source != D1L_ROUTE_SOURCE_NONE) {
        const uint32_t v2_epoch = v2_source == D1L_ROUTE_SOURCE_SD_V2 ?
                                  s_blob_scratch.epoch : s_fallback_blob_scratch.epoch;
        const uint32_t v2_total = v2_source == D1L_ROUTE_SOURCE_SD_V2 ?
                                  s_blob_scratch.total_written :
                                  s_fallback_blob_scratch.total_written;
        const uint32_t v2_count = v2_source == D1L_ROUTE_SOURCE_SD_V2 ?
                                  s_blob_scratch.count :
                                  s_fallback_blob_scratch.count;
        const uint32_t legacy_total = legacy_source == D1L_ROUTE_SOURCE_SD_LEGACY ?
                                      s_legacy_sd_blob_scratch.total_written :
                                      s_legacy_nvs_blob_scratch.total_written;
        const uint32_t legacy_count = legacy_source == D1L_ROUTE_SOURCE_SD_LEGACY ?
                                      s_legacy_sd_blob_scratch.count :
                                      s_legacy_nvs_blob_scratch.count;
        const bool compact_same_generation =
            v2_source == D1L_ROUTE_SOURCE_NVS_V2 && legacy_total == v2_total;
        if (v2_epoch == 1U &&
            (legacy_total > v2_total || compact_same_generation ||
             (legacy_total == v2_total && legacy_count > v2_count))) {
            source = legacy_source;
        }
    }

    const bool sd_v2_problem = sd_primary_required &&
        ((sd_ret == ESP_OK && !sd_valid) ||
         (sd_ret != ESP_OK && sd_ret != ESP_ERR_NOT_FOUND));
    const bool sd_legacy_problem = sd_primary_required &&
        ((legacy_sd_ret == ESP_OK && !legacy_sd_valid) ||
         (legacy_sd_ret != ESP_OK && legacy_sd_ret != ESP_ERR_NOT_FOUND));
    const bool selected_nvs_with_readable_sd = sd_primary_required &&
        (source == D1L_ROUTE_SOURCE_NVS_V2 ||
         source == D1L_ROUTE_SOURCE_NVS_LEGACY) &&
        (sd_valid || legacy_sd_valid);
    const bool sd_reconcile_pending = !sd_primary_required ||
                                      sd_v2_problem || sd_legacy_problem ||
                                      selected_nvs_with_readable_sd;
    const bool nvs_legacy_problem =
        (legacy_nvs_ret == ESP_OK && !legacy_nvs_valid) ||
        (legacy_nvs_ret != ESP_OK && legacy_nvs_ret != ESP_ERR_NOT_FOUND);
    const uint32_t selected_v2_epoch = v2_source == D1L_ROUTE_SOURCE_SD_V2 ?
        s_blob_scratch.epoch :
        v2_source == D1L_ROUTE_SOURCE_NVS_V2 ? s_fallback_blob_scratch.epoch : 0U;
    const bool unresolved_epoch_one_nvs_legacy = nvs_legacy_problem &&
        v2_source != D1L_ROUTE_SOURCE_NONE &&
        legacy_source == D1L_ROUTE_SOURCE_NONE && selected_v2_epoch == 1U;

    if ((source == D1L_ROUTE_SOURCE_NONE &&
         (sd_v2_problem || sd_legacy_problem || nvs_legacy_problem)) ||
        unresolved_epoch_one_nvs_legacy) {
        const esp_err_t failure = sd_v2_problem && sd_ret != ESP_OK ? sd_ret :
                                  sd_legacy_problem && legacy_sd_ret != ESP_OK ?
                                  legacy_sd_ret :
                                  nvs_legacy_problem && legacy_nvs_ret != ESP_OK ?
                                  legacy_nvs_ret : ESP_ERR_INVALID_STATE;
        d1l_store_lock_take(&s_store_lock);
        if (sd_v2_problem || sd_legacy_problem) {
            s_sd_primary_dirty = true;
            s_sd_reconcile_pending = true;
            s_sd_primary_fail_count++;
            s_sd_primary_last_error = failure;
        } else {
            s_nvs_fallback_dirty = true;
            s_nvs_fallback_fail_count++;
            s_nvs_fallback_last_error = failure;
        }
        s_persistence_fail_count++;
        s_persistence_dirty = true;
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return failure;
    }

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    d1l_store_lock_take(&s_store_lock);
    switch (source) {
    case D1L_ROUTE_SOURCE_SD_V2:
        load_valid_blob_into_ram();
        break;
    case D1L_ROUTE_SOURCE_NVS_V2:
        load_fallback_blob_into_ram(&s_fallback_blob_scratch);
        break;
    case D1L_ROUTE_SOURCE_SD_LEGACY:
        load_legacy_blob_into_ram(&s_legacy_sd_blob_scratch);
        break;
    case D1L_ROUTE_SOURCE_NVS_LEGACY:
        load_legacy_blob_into_ram(&s_legacy_nvs_blob_scratch);
        break;
    case D1L_ROUTE_SOURCE_NONE:
    default:
        break;
    }

    s_revision = 1U;
    s_mutated_since_init = false;
    s_mutations_since_init = 0U;
    s_boot_next_seq = s_next_seq;
    /* Once a valid source has been selected, any existing legacy NVS route
     * key is only rollback/cache material. It may be reclaimed later, and only
     * if allocating the compact v2 route key reports NVS exhaustion. */
    s_nvs_legacy_reclaim_allowed =
        source != D1L_ROUTE_SOURCE_NONE &&
        legacy_nvs_ret != ESP_ERR_NOT_FOUND;
    const bool loaded_legacy = source == D1L_ROUTE_SOURCE_SD_LEGACY ||
                               source == D1L_ROUTE_SOURCE_NVS_LEGACY;
    const uint32_t selected_epoch = s_epoch;
    const uint32_t selected_total = s_total_written;
    s_sd_reconcile_pending = sd_reconcile_pending;
    if (source == D1L_ROUTE_SOURCE_NONE) {
        s_sd_primary_dirty = false;
        s_nvs_fallback_dirty = false;
    } else {
        s_sd_primary_dirty = sd_primary_required &&
            (!sd_valid || s_blob_scratch.epoch != selected_epoch ||
             s_blob_scratch.total_written != selected_total);
        s_nvs_fallback_dirty =
            (!compact_valid || s_fallback_blob_scratch.epoch != selected_epoch ||
             s_fallback_blob_scratch.total_written != selected_total);
    }
    if (loaded_legacy) {
        s_sd_primary_dirty = sd_primary_required;
        s_nvs_fallback_dirty =
            !compact_valid || s_fallback_blob_scratch.epoch != selected_epoch ||
            s_fallback_blob_scratch.total_written != selected_total;
    }
    if (s_sd_primary_dirty || s_nvs_fallback_dirty ||
        (sd_primary_required && s_sd_reconcile_pending)) {
        note_persistence_dirty_locked(true, true, now_ms);
    }
    s_persistence_dirty = persistence_dirty_locked(sd_primary_required);
    s_loaded = true;
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    return ESP_OK;
}

esp_err_t d1l_route_store_init(void)
{
    return route_store_init_internal(true);
}

static esp_err_t ensure_route_store_initialized(void)
{
    d1l_store_lock_take(&s_store_lock);
    const bool loaded = s_loaded;
    d1l_store_lock_give(&s_store_lock);
    return loaded ? ESP_OK : route_store_init_internal(false);
}

esp_err_t d1l_route_store_clear(void)
{
    esp_err_t init_ret = ensure_route_store_initialized();
    if (init_ret != ESP_OK) {
        return init_ret;
    }
    d1l_store_lock_take(&s_persist_io_lock);
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    if (!d1l_retained_blob_store_backend_state(D1L_ROUTE_STORE_ID,
                                               &backend_state)) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool sd_primary_required = backend_state.enabled;
    d1l_store_lock_take(&s_store_lock);
    if (!s_loaded) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const uint64_t clear_revision = s_revision;
    const uint32_t clear_epoch = s_epoch + 1U;
    d1l_store_lock_give(&s_store_lock);

    esp_err_t sd_v2_erase = ESP_OK;
    esp_err_t sd_legacy_erase = ESP_OK;
    if (sd_primary_required) {
        if (!sd_backend_generation_matches(backend_state.generation)) {
            sd_v2_erase = ESP_ERR_INVALID_STATE;
        } else {
            sd_v2_erase = d1l_retained_blob_store_erase_sd_primary_guarded(
                D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_V2_KEY,
                backend_state.generation);
            sd_legacy_erase = d1l_retained_blob_store_erase_sd_primary_guarded(
                D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_LEGACY_KEY,
                backend_state.generation);
            if (!sd_backend_generation_matches(backend_state.generation)) {
                sd_v2_erase = ESP_ERR_INVALID_STATE;
            }
        }
    }
    const esp_err_t nvs_v2_erase = d1l_retained_blob_store_erase_nvs_fallback(
        D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_V2_KEY);
    const esp_err_t nvs_legacy_erase = d1l_retained_blob_store_erase_nvs_fallback(
        D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_LEGACY_KEY);

    esp_err_t ret = sd_v2_erase != ESP_OK ? sd_v2_erase :
                    sd_legacy_erase != ESP_OK ? sd_legacy_erase :
                    nvs_v2_erase != ESP_OK ? nvs_v2_erase : nvs_legacy_erase;
    esp_err_t sd_write = ESP_OK;
    esp_err_t nvs_write = ESP_OK;
    if (ret == ESP_OK) {
        memset(&s_persist_snapshot.primary, 0, sizeof(s_persist_snapshot.primary));
        s_persist_snapshot.primary.schema = D1L_ROUTE_STORE_FULL_SCHEMA;
        s_persist_snapshot.primary.epoch = clear_epoch;
        s_persist_snapshot.primary.next_seq = 1U;
        memset(&s_persist_snapshot.fallback, 0, sizeof(s_persist_snapshot.fallback));
        s_persist_snapshot.fallback.schema = D1L_ROUTE_STORE_FALLBACK_SCHEMA;
        s_persist_snapshot.fallback.epoch = clear_epoch;
        s_persist_snapshot.fallback.next_seq = 1U;
        if (sd_primary_required &&
            sd_backend_generation_matches(backend_state.generation)) {
            sd_write = d1l_retained_blob_store_write_sd_primary_guarded(
                D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_V2_KEY,
                &s_persist_snapshot.primary, sizeof(s_persist_snapshot.primary),
                backend_state.generation);
            if (!sd_backend_generation_matches(backend_state.generation)) {
                sd_write = ESP_ERR_INVALID_STATE;
            }
        } else if (sd_primary_required) {
            sd_write = ESP_ERR_INVALID_STATE;
        }
        nvs_write = d1l_retained_blob_store_write_nvs_fallback(
            D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_V2_KEY,
            &s_persist_snapshot.fallback, sizeof(s_persist_snapshot.fallback));
        ret = sd_write != ESP_OK ? sd_write : nvs_write;
    }

    d1l_store_lock_take(&s_store_lock);
    const bool same_revision = s_revision == clear_revision;
    if (ret == ESP_OK && same_revision) {
        clear_ram();
        s_epoch = clear_epoch;
        s_revision++;
        s_loaded = true;
        s_persistence_dirty = false;
        s_persistence_material_dirty = false;
        s_sd_primary_dirty = false;
        s_sd_reconcile_pending = !sd_primary_required;
        s_last_sd_primary_required = sd_primary_required;
        s_last_sd_backend_generation = backend_state.generation;
        s_nvs_fallback_dirty = false;
        s_mutated_since_init = false;
        s_mutations_since_init = 0U;
        s_boot_next_seq = s_next_seq;
        s_retry_pending = false;
        clear_persistence_timing_locked();
        s_persisted_this_boot = true;
        s_persist_attempted_this_boot = true;
        s_last_persist_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        s_clear_failure_latched = false;
        s_clear_last_error = ESP_OK;
        s_nvs_legacy_reclaim_allowed = false;
        if (sd_primary_required) {
            s_sd_primary_commit_count++;
            s_sd_primary_last_error = ESP_OK;
        }
        s_nvs_fallback_commit_count++;
        s_nvs_fallback_last_error = ESP_OK;
        s_persistence_commit_count++;
    } else {
        const esp_err_t failure = ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
        s_clear_failure_latched = true;
        s_clear_last_error = failure;
        s_clear_fail_count++;
        s_persistence_fail_count++;
        s_retry_pending = true;
        s_sd_primary_dirty = sd_primary_required;
        s_nvs_fallback_dirty = true;
        note_persistence_dirty_locked(true, true,
                                      (uint32_t)(esp_timer_get_time() / 1000ULL));
        if (sd_primary_required &&
            (sd_v2_erase != ESP_OK || sd_legacy_erase != ESP_OK || sd_write != ESP_OK)) {
            s_sd_primary_fail_count++;
            s_sd_primary_last_error = failure;
        }
        if (nvs_v2_erase != ESP_OK || nvs_legacy_erase != ESP_OK || nvs_write != ESP_OK) {
            s_nvs_fallback_fail_count++;
            s_nvs_fallback_last_error = failure;
        }
        ret = failure;
    }
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    return ret;
}

esp_err_t d1l_route_store_clear_target(const char *target)
{
    if (!target || target[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t init_ret = ensure_route_store_initialized();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    d1l_store_lock_take(&s_store_lock);
    bool changed = false;
    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < s_count; ++read_index) {
        if (strncmp(s_entries[read_index].target, target,
                    sizeof(s_entries[read_index].target)) == 0) {
            changed = true;
            continue;
        }
        if (write_index != read_index) {
            s_entries[write_index] = s_entries[read_index];
        }
        write_index++;
    }
    if (write_index < s_count) {
        memset(&s_entries[write_index], 0,
               (s_count - write_index) * sizeof(s_entries[0]));
        s_count = write_index;
    }
    if (s_volatile_valid &&
        strncmp(s_volatile_entry.target, target,
                sizeof(s_volatile_entry.target)) == 0) {
        memset(&s_volatile_entry, 0, sizeof(s_volatile_entry));
        s_volatile_valid = false;
        changed = true;
    }
    if (changed) {
        const uint32_t now_ms =
            (uint32_t)(esp_timer_get_time() / 1000ULL);
        s_revision++;
        s_mutated_since_init = true;
        s_mutations_since_init++;
        s_sd_primary_dirty = true;
        s_nvs_fallback_dirty = true;
        note_persistence_dirty_locked(true, false, now_ms);
        s_persistence_coalesced_count++;
    }
    d1l_store_lock_give(&s_store_lock);
    if (changed) {
        const esp_err_t retire_ret =
            d1l_retained_blob_store_erase_nvs_fallback(
                D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_V2_KEY);
        if (retire_ret != ESP_OK) {
            return retire_ret;
        }
        (void)d1l_retained_blob_store_erase_nvs_fallback(
            D1L_ROUTE_STORE_ID, D1L_ROUTE_STORE_LEGACY_KEY);
    }
    return ESP_OK;
}

esp_err_t d1l_route_store_flush(void)
{
    esp_err_t ret = ensure_route_store_initialized();
    if (ret != ESP_OK) {
        return ret;
    }
    return persist_route_snapshot(true);
}

esp_err_t d1l_route_store_flush_if_due(void)
{
    esp_err_t ret = ensure_route_store_initialized();
    if (ret != ESP_OK) {
        return ret;
    }
    return persist_route_snapshot(false);
}

static bool route_materially_changed(const d1l_route_entry_t *before,
                                     const d1l_route_entry_t *after)
{
    return !before || !after ||
           strncmp(before->target, after->target, sizeof(after->target)) != 0 ||
           strncmp(before->label, after->label, sizeof(after->label)) != 0 ||
           strncmp(before->kind, after->kind, sizeof(after->kind)) != 0 ||
           strncmp(before->route, after->route, sizeof(after->route)) != 0 ||
           strncmp(before->direction, after->direction, sizeof(after->direction)) != 0 ||
           before->path_hash_bytes != after->path_hash_bytes ||
           before->path_hops != after->path_hops ||
           before->payload_len != after->payload_len;
}

static esp_err_t upsert_observation_internal(const char *target, const char *label,
                                             const char *kind, const char *route,
                                             const char *direction, int rssi_dbm,
                                             int snr_tenths, uint8_t path_hash_bytes,
                                             uint8_t path_hops, uint16_t payload_len,
                                             bool persist, bool defer_lock)
{
    if (!target || target[0] == '\0' ||
        path_hash_bytes == 0U || path_hash_bytes > 3U ||
        path_hops > 63U ||
        (uint16_t)path_hash_bytes * path_hops > 64U ||
        payload_len > D1L_ROUTE_STORE_MAX_PAYLOAD_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (defer_lock) {
        if (!d1l_store_lock_try_take(&s_store_lock)) {
            return ESP_ERR_TIMEOUT;
        }
    } else {
        d1l_store_lock_take(&s_store_lock);
    }
    if (!s_loaded) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (!persist) {
        const bool same_volatile = s_volatile_valid &&
            strncmp(s_volatile_entry.target, target,
                    sizeof(s_volatile_entry.target)) == 0 &&
            strncmp(s_volatile_entry.kind, kind ? kind : "",
                    sizeof(s_volatile_entry.kind)) == 0 &&
            strncmp(s_volatile_entry.direction, direction ? direction : "",
                    sizeof(s_volatile_entry.direction)) == 0;
        if (!same_volatile) {
            memset(&s_volatile_entry, 0, sizeof(s_volatile_entry));
            s_volatile_entry.first_seen_ms = now_ms;
        } else if (now_ms < s_volatile_entry.first_seen_ms) {
            s_volatile_entry.first_seen_ms = now_ms;
        }
        s_volatile_entry.seq = s_next_seq;
        s_volatile_entry.last_seen_ms = now_ms;
        s_volatile_entry.seen_count++;
        s_volatile_entry.last_rssi_dbm = rssi_dbm;
        s_volatile_entry.last_snr_tenths = snr_tenths;
        s_volatile_entry.path_hash_bytes = path_hash_bytes;
        s_volatile_entry.path_hops = path_hops;
        s_volatile_entry.confidence = route_confidence(path_hops);
        s_volatile_entry.payload_len = payload_len;
        sanitize_ascii(s_volatile_entry.target, sizeof(s_volatile_entry.target), target);
        sanitize_ascii(s_volatile_entry.label, sizeof(s_volatile_entry.label),
                       label && label[0] ? label : target);
        sanitize_ascii(s_volatile_entry.kind, sizeof(s_volatile_entry.kind),
                       kind && kind[0] ? kind : "packet");
        sanitize_ascii(s_volatile_entry.route, sizeof(s_volatile_entry.route),
                       route && route[0] ? route : "unknown");
        sanitize_ascii(s_volatile_entry.direction, sizeof(s_volatile_entry.direction),
                       direction && direction[0] ? direction : "rx");
        s_volatile_valid = true;
        d1l_store_lock_give(&s_store_lock);
        return ESP_OK;
    }

    /* A UI-only canary borrows next_seq without consuming durable capacity or
     * metadata. The next real observation supersedes that preview slot. */
    s_volatile_valid = false;
    int existing = find_index(target, kind, direction);
    size_t index;
    bool is_new = existing < 0;
    if (!is_new) {
        index = (size_t)existing;
    } else if (s_count < D1L_ROUTE_STORE_CAPACITY) {
        index = s_count++;
    } else {
        index = oldest_index();
        s_dropped_oldest++;
    }

    d1l_route_entry_t *entry = &s_entries[index];
    const d1l_route_entry_t before = *entry;
    if (is_new) {
        memset(entry, 0, sizeof(*entry));
        entry->first_seen_ms = now_ms;
    } else if (now_ms < entry->first_seen_ms) {
        entry->first_seen_ms = now_ms;
    }
    entry->seq = s_next_seq++;
    entry->last_seen_ms = now_ms;
    entry->seen_count++;
    entry->last_rssi_dbm = rssi_dbm;
    entry->last_snr_tenths = snr_tenths;
    entry->path_hash_bytes = path_hash_bytes;
    entry->path_hops = path_hops;
    entry->confidence = route_confidence(path_hops);
    entry->payload_len = payload_len;
    sanitize_ascii(entry->target, sizeof(entry->target), target);
    sanitize_ascii(entry->label, sizeof(entry->label), label && label[0] ? label : target);
    sanitize_ascii(entry->kind, sizeof(entry->kind), kind && kind[0] ? kind : "packet");
    sanitize_ascii(entry->route, sizeof(entry->route), route && route[0] ? route : "unknown");
    sanitize_ascii(entry->direction, sizeof(entry->direction), direction && direction[0] ? direction : "rx");
    s_total_written++;
    const bool material_change = is_new || route_materially_changed(&before, entry);
    s_revision++;
    s_mutated_since_init = true;
    s_mutations_since_init++;
    s_sd_primary_dirty = true;
    s_nvs_fallback_dirty = true;
    note_persistence_dirty_locked(material_change, false, now_ms);
    s_persistence_coalesced_count++;
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

esp_err_t d1l_route_store_upsert_observation(const char *target, const char *label,
                                             const char *kind, const char *route,
                                             const char *direction, int rssi_dbm,
                                             int snr_tenths, uint8_t path_hash_bytes,
                                             uint8_t path_hops, uint16_t payload_len)
{
    return upsert_observation_internal(target, label, kind, route, direction, rssi_dbm,
                                       snr_tenths, path_hash_bytes, path_hops,
                                       payload_len, true, false);
}

esp_err_t d1l_route_store_upsert_observation_deferred(
    const char *target, const char *label, const char *kind,
    const char *route, const char *direction, int rssi_dbm,
    int snr_tenths, uint8_t path_hash_bytes, uint8_t path_hops,
    uint16_t payload_len)
{
    return upsert_observation_internal(
        target, label, kind, route, direction, rssi_dbm, snr_tenths,
        path_hash_bytes, path_hops, payload_len, true, true);
}

esp_err_t d1l_route_store_upsert_observation_volatile(const char *target, const char *label,
                                                      const char *kind, const char *route,
                                                      const char *direction, int rssi_dbm,
                                                      int snr_tenths, uint8_t path_hash_bytes,
                                                      uint8_t path_hops, uint16_t payload_len)
{
    return upsert_observation_internal(target, label, kind, route, direction, rssi_dbm,
                                       snr_tenths, path_hash_bytes, path_hops,
                                       payload_len, false, false);
}

d1l_route_store_stats_t d1l_route_store_stats(void)
{
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    (void)d1l_retained_blob_store_backend_state(D1L_ROUTE_STORE_ID,
                                                &backend_state);
    const bool sd_primary_required = backend_state.enabled;
    d1l_store_lock_take(&s_store_lock);
    s_persistence_dirty = persistence_dirty_locked(sd_primary_required);
    d1l_route_store_stats_t stats = {
        .next_seq = s_next_seq,
        .total_written = s_total_written,
        .dropped_oldest = s_dropped_oldest,
        .persistence_commit_count = s_persistence_commit_count,
        .persistence_coalesced_count = s_persistence_coalesced_count,
        .persistence_fail_count = s_persistence_fail_count,
        .persistence_stale_snapshot_count = s_persistence_stale_snapshot_count,
        .sd_primary_commit_count = s_sd_primary_commit_count,
        .sd_primary_fail_count = s_sd_primary_fail_count,
        .sd_backend_generation = backend_state.generation,
        .sd_primary_last_error = s_sd_primary_last_error,
        .nvs_fallback_commit_count = s_nvs_fallback_commit_count,
        .nvs_fallback_fail_count = s_nvs_fallback_fail_count,
        .nvs_fallback_last_error = s_nvs_fallback_last_error,
        .clear_fail_count = s_clear_fail_count,
        .clear_last_error = s_clear_last_error,
        .persistence_revision = s_revision,
        .count = s_count,
        .capacity = D1L_ROUTE_STORE_CAPACITY,
        .persistence_dirty = s_persistence_dirty,
        .sd_primary_required = sd_primary_required,
        .sd_primary_dirty = s_sd_primary_dirty,
        .sd_primary_reconcile_pending = s_sd_reconcile_pending,
        .nvs_fallback_dirty = s_nvs_fallback_dirty,
        .clear_failure_latched = s_clear_failure_latched,
    };
    d1l_store_lock_give(&s_store_lock);
    return stats;
}

size_t d1l_route_store_copy_recent(d1l_route_entry_t *out_entries, size_t max_entries)
{
    if (out_entries == NULL || max_entries == 0) {
        return 0;
    }

    d1l_store_lock_take(&s_store_lock);
    if (s_count == 0 && !s_volatile_valid) {
        d1l_store_lock_give(&s_store_lock);
        return 0;
    }
    const size_t visible_limit = max_entries < D1L_ROUTE_STORE_CAPACITY ?
                                 max_entries : D1L_ROUTE_STORE_CAPACITY;
    size_t copied = 0;
    if (s_volatile_valid && copied < visible_limit) {
        out_entries[copied++] = s_volatile_entry;
    }
    const size_t durable_limit = s_count < (visible_limit - copied) ?
                                 s_count : (visible_limit - copied);
    bool used[D1L_ROUTE_STORE_CAPACITY] = {0};
    for (size_t out = 0; out < durable_limit; ++out) {
        size_t best = 0;
        bool best_set = false;
        for (size_t i = 0; i < s_count; ++i) {
            if (!used[i] && (!best_set || s_entries[i].seq > s_entries[best].seq)) {
                best = i;
                best_set = true;
            }
        }
        used[best] = true;
        out_entries[copied++] = s_entries[best];
    }
    d1l_store_lock_give(&s_store_lock);
    return copied;
}

size_t d1l_route_store_copy_for_target(const char *target, d1l_route_entry_t *out_entries,
                                       size_t max_entries)
{
    if (!target || target[0] == '\0' || out_entries == NULL || max_entries == 0) {
        return 0;
    }

    d1l_store_lock_take(&s_store_lock);
    if (s_count == 0 && !s_volatile_valid) {
        d1l_store_lock_give(&s_store_lock);
        return 0;
    }
    size_t copied = 0;
    const size_t visible_limit = max_entries < D1L_ROUTE_STORE_CAPACITY ?
                                 max_entries : D1L_ROUTE_STORE_CAPACITY;
    if (s_volatile_valid &&
        strncmp(s_volatile_entry.target, target,
                sizeof(s_volatile_entry.target)) == 0 &&
        copied < visible_limit) {
        out_entries[copied++] = s_volatile_entry;
    }
    bool used[D1L_ROUTE_STORE_CAPACITY] = {0};
    while (copied < visible_limit) {
        size_t best = 0;
        bool best_set = false;
        for (size_t i = 0; i < s_count; ++i) {
            if (used[i] ||
                strncmp(s_entries[i].target, target, sizeof(s_entries[i].target)) != 0) {
                continue;
            }
            if (!best_set || s_entries[i].seq > s_entries[best].seq) {
                best = i;
                best_set = true;
            }
        }
        if (!best_set) {
            break;
        }
        used[best] = true;
        out_entries[copied++] = s_entries[best];
    }
    d1l_store_lock_give(&s_store_lock);
    return copied;
}

esp_err_t d1l_route_store_find_by_seq(uint32_t seq, d1l_route_entry_t *out_entry)
{
    if (seq == 0 || out_entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_store_lock_take(&s_store_lock);
    if (!s_loaded) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_volatile_valid && s_volatile_entry.seq == seq) {
        *out_entry = s_volatile_entry;
        d1l_store_lock_give(&s_store_lock);
        return ESP_OK;
    }
    for (size_t i = 0; i < s_count; ++i) {
        if (s_entries[i].seq == seq) {
            *out_entry = s_entries[i];
            d1l_store_lock_give(&s_store_lock);
            return ESP_OK;
        }
    }
    d1l_store_lock_give(&s_store_lock);
    return ESP_ERR_NOT_FOUND;
}
