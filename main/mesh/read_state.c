#include "read_state.h"

#include <stdbool.h>
#include <string.h>

#include "esp_attr.h"

#include "mesh/contact_store.h"
#include "mesh/dm_store.h"
#include "mesh/message_store.h"
#include "mesh/store_lock.h"
#include "storage/retained_blob_store.h"

#define D1L_READ_STATE_KEY "state"
#define D1L_READ_STATE_SCHEMA 2U
#define D1L_READ_STATE_SCHEMA_V1 1U
/* The durable DM ring can expose one additional volatile row while its
 * persistence retry is pending.  Read cursors and global counts must include
 * that visible tail even though only 16 durable rows/cursors are retained. */
#define D1L_READ_STATE_VISIBLE_DM_CAPACITY (D1L_DM_STORE_CAPACITY + 1U)

typedef struct {
    uint32_t schema;
    uint32_t last_public_read_seq;
    uint32_t last_dm_read_seq;
    uint32_t mark_read_count;
} d1l_read_state_v1_blob_t;

typedef struct {
    uint32_t schema;
    uint32_t last_public_read_seq;
    uint32_t last_dm_read_seq;
    uint32_t mark_read_count;
    uint32_t dm_cursor_count;
    d1l_read_state_persisted_dm_cursor_t
        dm_cursors[D1L_READ_STATE_DM_THREAD_CAPACITY];
} d1l_read_state_v2_blob_t;

typedef enum {
    D1L_READ_STATE_AUTHORITY_NONE = 0,
    D1L_READ_STATE_AUTHORITY_LOCAL,
    D1L_READ_STATE_AUTHORITY_EXPLICIT_CLEAR,
} d1l_read_state_authority_t;

typedef struct {
    d1l_read_state_v2_blob_t state;
    d1l_read_state_authority_t authority;
    uint64_t persistence_revision;
    bool sd_primary_dirty;
    bool sd_reconcile_pending;
    bool nvs_fallback_dirty;
} d1l_read_state_runtime_snapshot_t;

static d1l_read_state_v2_blob_t s_state;
static bool s_loaded;
static d1l_read_state_authority_t s_authority;
static uint32_t s_accepted_sd_backend_generation;
static uint64_t s_persistence_revision;
static uint32_t s_persistence_commit_count;
static uint32_t s_persistence_fail_count;
static esp_err_t s_persistence_last_error;
static bool s_sd_primary_dirty;
static bool s_sd_reconcile_pending;
static bool s_nvs_fallback_dirty;
static d1l_store_lock_t s_store_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_persist_io_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_projection_lock = D1L_STORE_LOCK_INITIALIZER;
#ifdef D1L_READ_STATE_TEST_HOOKS
static void (*s_after_sd_read_hook)(void);
#endif
static d1l_message_entry_t s_message_scratch[D1L_MESSAGE_STORE_CAPACITY]
    EXT_RAM_BSS_ATTR;
static d1l_dm_entry_t s_dm_scratch[D1L_READ_STATE_VISIBLE_DM_CAPACITY]
    EXT_RAM_BSS_ATTR;
static d1l_read_state_dm_thread_t
    s_thread_scratch[D1L_READ_STATE_VISIBLE_DM_CAPACITY] EXT_RAM_BSS_ATTR;

static void clear_blob(d1l_read_state_v2_blob_t *blob)
{
    if (!blob) {
        return;
    }
    memset(blob, 0, sizeof(*blob));
    blob->schema = D1L_READ_STATE_SCHEMA;
}

static void clear_ram(void)
{
    clear_blob(&s_state);
}

static void normalize_blob(d1l_read_state_v2_blob_t *blob)
{
    if (!blob) {
        return;
    }
    blob->schema = D1L_READ_STATE_SCHEMA;
    if (blob->dm_cursor_count > D1L_READ_STATE_DM_THREAD_CAPACITY) {
        blob->dm_cursor_count = D1L_READ_STATE_DM_THREAD_CAPACITY;
    }
}

static bool blob_v1_is_valid(const d1l_read_state_v1_blob_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) && blob->schema == D1L_READ_STATE_SCHEMA_V1;
}

static bool blob_v2_is_valid(const d1l_read_state_v2_blob_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_READ_STATE_SCHEMA &&
           blob->dm_cursor_count <= D1L_READ_STATE_DM_THREAD_CAPACITY;
}

static esp_err_t decode_blob(const void *raw, size_t len,
                             d1l_read_state_v2_blob_t *out_blob,
                             bool *out_migrated)
{
    if (!raw || !out_blob) {
        return ESP_ERR_INVALID_ARG;
    }
    clear_blob(out_blob);
    if (out_migrated) {
        *out_migrated = false;
    }
    if (len == sizeof(d1l_read_state_v2_blob_t)) {
        const d1l_read_state_v2_blob_t *blob = raw;
        if (!blob_v2_is_valid(blob, len)) {
            return ESP_ERR_INVALID_VERSION;
        }
        *out_blob = *blob;
        return ESP_OK;
    }
    if (len == sizeof(d1l_read_state_v1_blob_t)) {
        const d1l_read_state_v1_blob_t *legacy = raw;
        if (!blob_v1_is_valid(legacy, len)) {
            return ESP_ERR_INVALID_VERSION;
        }
        out_blob->last_public_read_seq = legacy->last_public_read_seq;
        out_blob->last_dm_read_seq = legacy->last_dm_read_seq;
        out_blob->mark_read_count = legacy->mark_read_count;
        if (out_migrated) {
            *out_migrated = true;
        }
        return ESP_OK;
    }
    return ESP_ERR_INVALID_SIZE;
}

static bool blob_is_empty(const d1l_read_state_v2_blob_t *blob)
{
    return blob && blob->last_public_read_seq == 0U &&
           blob->last_dm_read_seq == 0U && blob->mark_read_count == 0U &&
           blob->dm_cursor_count == 0U;
}

static bool same_fingerprint(const char *lhs, const char *rhs)
{
    return lhs && rhs &&
           strncmp(lhs, rhs, D1L_NODE_FINGERPRINT_LEN) == 0;
}

static int find_cursor_in_blob(const d1l_read_state_v2_blob_t *blob,
                               const char *fingerprint)
{
    if (!blob || !fingerprint || fingerprint[0] == '\0') {
        return -1;
    }
    for (uint32_t i = 0U; i < blob->dm_cursor_count; ++i) {
        if (same_fingerprint(blob->dm_cursors[i].fingerprint, fingerprint)) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t merge_monotonic(
    const d1l_read_state_v2_blob_t *local,
    const d1l_read_state_v2_blob_t *sd,
    d1l_read_state_v2_blob_t *out_merged)
{
    if (!local || !sd || !out_merged ||
        !blob_v2_is_valid(local, sizeof(*local)) ||
        !blob_v2_is_valid(sd, sizeof(*sd))) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_merged = *local;
    if (sd->last_public_read_seq > out_merged->last_public_read_seq) {
        out_merged->last_public_read_seq = sd->last_public_read_seq;
    }
    if (sd->last_dm_read_seq > out_merged->last_dm_read_seq) {
        out_merged->last_dm_read_seq = sd->last_dm_read_seq;
    }
    if (sd->mark_read_count > out_merged->mark_read_count) {
        out_merged->mark_read_count = sd->mark_read_count;
    }
    for (uint32_t i = 0U; i < sd->dm_cursor_count; ++i) {
        const d1l_read_state_persisted_dm_cursor_t *sd_cursor =
            &sd->dm_cursors[i];
        const int local_idx = find_cursor_in_blob(
            out_merged, sd_cursor->fingerprint);
        if (local_idx >= 0) {
            d1l_read_state_persisted_dm_cursor_t *merged_cursor =
                &out_merged->dm_cursors[local_idx];
            if (sd_cursor->last_read_seq > merged_cursor->last_read_seq) {
                merged_cursor->last_read_seq = sd_cursor->last_read_seq;
            }
            continue;
        }
        if (out_merged->dm_cursor_count >=
            D1L_READ_STATE_DM_THREAD_CAPACITY) {
            /* Dropping either side would move a per-fingerprint cursor
             * backwards. Leave both durable copies untouched instead. */
            return ESP_ERR_NO_MEM;
        }
        out_merged->dm_cursors[out_merged->dm_cursor_count++] = *sd_cursor;
    }
    normalize_blob(out_merged);
    return ESP_OK;
}

static void capture_runtime(d1l_read_state_runtime_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    snapshot->state = s_state;
    snapshot->authority = s_authority;
    snapshot->persistence_revision = s_persistence_revision;
    snapshot->sd_primary_dirty = s_sd_primary_dirty;
    snapshot->sd_reconcile_pending = s_sd_reconcile_pending;
    snapshot->nvs_fallback_dirty = s_nvs_fallback_dirty;
}

static void restore_runtime(
    const d1l_read_state_runtime_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    s_state = snapshot->state;
    s_authority = snapshot->authority;
    s_persistence_revision = snapshot->persistence_revision;
    s_sd_primary_dirty = snapshot->sd_primary_dirty;
    s_sd_reconcile_pending = snapshot->sd_reconcile_pending;
    s_nvs_fallback_dirty = snapshot->nvs_fallback_dirty;
}

static void note_persistence_commit(void)
{
    if (s_persistence_commit_count < UINT32_MAX) {
        s_persistence_commit_count++;
    }
    s_persistence_last_error = ESP_OK;
}

static void note_persistence_failure(esp_err_t error)
{
    if (s_persistence_fail_count < UINT32_MAX) {
        s_persistence_fail_count++;
    }
    s_persistence_last_error = error;
}

static esp_err_t reserve_persistence_revision(void)
{
    if (s_persistence_revision == UINT64_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    s_persistence_revision++;
    return ESP_OK;
}

static bool sd_backend_generation_matches(uint32_t expected_generation)
{
    d1l_retained_blob_store_backend_state_t backend = {0};
    return d1l_retained_blob_store_backend_state(
               D1L_RETAINED_BLOB_STORE_READ_STATE, &backend) &&
           backend.enabled && backend.generation == expected_generation;
}

static esp_err_t read_fallback_blob(d1l_read_state_v2_blob_t *out_blob,
                                    bool *out_found, bool *out_migrated)
{
    if (!out_blob || !out_found || !out_migrated) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_found = false;
    *out_migrated = false;
    d1l_read_state_v2_blob_t raw = {0};
    size_t len = sizeof(raw);
    esp_err_t ret = d1l_retained_blob_store_read_nvs_fallback(
        D1L_RETAINED_BLOB_STORE_READ_STATE, D1L_READ_STATE_KEY,
        &raw, &len);
    if (ret == ESP_ERR_NOT_FOUND) {
        clear_blob(out_blob);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    ret = decode_blob(&raw, len, out_blob, out_migrated);
    if (ret == ESP_OK) {
        *out_found = true;
    }
    return ret;
}

static esp_err_t read_current_sd_blob(
    uint32_t expected_generation, d1l_read_state_v2_blob_t *out_blob,
    bool *out_found)
{
    if (!out_blob || !out_found ||
        !sd_backend_generation_matches(expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_found = false;
    d1l_read_state_v2_blob_t raw = {0};
    size_t len = sizeof(raw);
    const esp_err_t read_ret = d1l_retained_blob_store_read_sd_primary(
        D1L_RETAINED_BLOB_STORE_READ_STATE, D1L_READ_STATE_KEY,
        &raw, &len);
#ifdef D1L_READ_STATE_TEST_HOOKS
    void (*after_read_hook)(void) = s_after_sd_read_hook;
    s_after_sd_read_hook = NULL;
    if (after_read_hook) {
        after_read_hook();
    }
#endif
    if (!sd_backend_generation_matches(expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (read_ret == ESP_ERR_NOT_FOUND) {
        clear_blob(out_blob);
        return ESP_OK;
    }
    if (read_ret != ESP_OK) {
        return read_ret;
    }
    const esp_err_t decode_ret = decode_blob(&raw, len, out_blob, NULL);
    if (decode_ret != ESP_OK) {
        return decode_ret;
    }
    if (!sd_backend_generation_matches(expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_found = true;
    return ESP_OK;
}

static esp_err_t write_fallback_journal(void)
{
    normalize_blob(&s_state);
    const esp_err_t ret = d1l_retained_blob_store_write_nvs_fallback(
        D1L_RETAINED_BLOB_STORE_READ_STATE, D1L_READ_STATE_KEY,
        &s_state, sizeof(s_state));
    if (ret == ESP_OK) {
        s_nvs_fallback_dirty = false;
    }
    return ret;
}

static esp_err_t reconcile_current_sd(uint32_t expected_generation)
{
    const uint64_t expected_revision = s_persistence_revision;
    d1l_read_state_v2_blob_t sd_blob = {0};
    bool sd_found = false;
    esp_err_t ret = read_current_sd_blob(
        expected_generation, &sd_blob, &sd_found);
    if (ret != ESP_OK) {
        s_sd_reconcile_pending = true;
        return ret;
    }
    if (s_persistence_revision != expected_revision) {
        s_sd_reconcile_pending = true;
        return ESP_ERR_INVALID_STATE;
    }

    d1l_read_state_v2_blob_t replacement = {0};
    bool write_needed = false;
    bool erase_needed = false;
    if (s_authority == D1L_READ_STATE_AUTHORITY_EXPLICIT_CLEAR) {
        clear_blob(&replacement);
        /* A guarded erase also removes recovery/temporary SD candidates, so
         * perform it even when the primary path was reported absent. */
        erase_needed = true;
    } else if (s_authority == D1L_READ_STATE_AUTHORITY_LOCAL) {
        replacement = s_state;
        if (sd_found) {
            ret = merge_monotonic(&s_state, &sd_blob, &replacement);
            if (ret != ESP_OK) {
                s_sd_reconcile_pending = true;
                return ret;
            }
        }
        write_needed =
            !sd_found ||
            memcmp(&replacement, &sd_blob, sizeof(replacement)) != 0;
    } else if (sd_found) {
        replacement = sd_blob;
    } else {
        clear_blob(&replacement);
    }
    const bool ram_replacement =
        memcmp(&replacement, &s_state, sizeof(replacement)) != 0;
    if (ram_replacement && s_persistence_revision == UINT64_MAX) {
        s_sd_reconcile_pending = true;
        return ESP_ERR_INVALID_STATE;
    }

    if (!sd_backend_generation_matches(expected_generation) ||
        s_persistence_revision != expected_revision) {
        s_sd_reconcile_pending = true;
        return ESP_ERR_INVALID_STATE;
    }
    if (erase_needed) {
        ret = d1l_retained_blob_store_erase_sd_primary_guarded(
            D1L_RETAINED_BLOB_STORE_READ_STATE, D1L_READ_STATE_KEY,
            expected_generation);
    } else if (write_needed) {
        ret = d1l_retained_blob_store_write_sd_primary_guarded(
            D1L_RETAINED_BLOB_STORE_READ_STATE, D1L_READ_STATE_KEY,
            &replacement, sizeof(replacement), expected_generation);
    }
    if (ret != ESP_OK ||
        !sd_backend_generation_matches(expected_generation) ||
        s_persistence_revision != expected_revision) {
        s_sd_reconcile_pending = true;
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }

    if (s_authority != D1L_READ_STATE_AUTHORITY_NONE) {
        ret = d1l_retained_blob_store_erase_nvs_fallback(
            D1L_RETAINED_BLOB_STORE_READ_STATE, D1L_READ_STATE_KEY);
        if (ret != ESP_OK) {
            s_nvs_fallback_dirty = true;
            s_sd_reconcile_pending = true;
            return ret;
        }
        if (s_persistence_revision != expected_revision) {
            s_sd_reconcile_pending = true;
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* Do not apply an SD replacement to RAM until the complete read/write
     * sequence is fenced to one still-current backend generation. */
    if (!sd_backend_generation_matches(expected_generation) ||
        s_persistence_revision != expected_revision) {
        if (s_authority != D1L_READ_STATE_AUTHORITY_NONE) {
            /* The fallback was retired only moments ago. Recreate the local
             * journal/tombstone if the SD fence moved during that retirement
             * so a reboot cannot lose the still-authoritative local state. */
            s_nvs_fallback_dirty = true;
            (void)write_fallback_journal();
        }
        s_sd_reconcile_pending = true;
        return ESP_ERR_INVALID_STATE;
    }
    s_state = replacement;
    if (ram_replacement) {
        s_persistence_revision++;
    }
    s_accepted_sd_backend_generation = expected_generation;
    s_authority = D1L_READ_STATE_AUTHORITY_NONE;
    s_sd_primary_dirty = false;
    s_sd_reconcile_pending = false;
    s_nvs_fallback_dirty = false;
    if (write_needed || erase_needed) {
        note_persistence_commit();
    }
    return ESP_OK;
}

static esp_err_t flush_loaded(void)
{
    d1l_retained_blob_store_backend_state_t backend = {0};
    if (!d1l_retained_blob_store_backend_state(
            D1L_RETAINED_BLOB_STORE_READ_STATE, &backend)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_nvs_fallback_dirty) {
        const esp_err_t journal_ret = write_fallback_journal();
        if (journal_ret != ESP_OK) {
            return journal_ret;
        }
    }
    if (!backend.enabled) {
        if (s_authority != D1L_READ_STATE_AUTHORITY_NONE ||
            s_sd_primary_dirty) {
            s_sd_reconcile_pending = true;
        }
        return ESP_OK;
    }
    if (backend.generation != s_accepted_sd_backend_generation) {
        s_sd_reconcile_pending = true;
    }
    if (!s_sd_reconcile_pending && !s_sd_primary_dirty) {
        return ESP_OK;
    }
    return reconcile_current_sd(backend.generation);
}

static esp_err_t persist_mutation_or_rollback(
    const d1l_read_state_runtime_snapshot_t *previous,
    d1l_read_state_authority_t authority)
{
    if (!previous || authority == D1L_READ_STATE_AUTHORITY_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    normalize_blob(&s_state);
    s_authority = authority;
    s_sd_primary_dirty = true;
    s_sd_reconcile_pending = true;
    s_nvs_fallback_dirty = true;
    const uint64_t expected_revision = s_persistence_revision;

    esp_err_t ret = write_fallback_journal();
    if (ret != ESP_OK) {
        restore_runtime(previous);
        note_persistence_failure(ret);
        return ret;
    }
    if (s_persistence_revision != expected_revision) {
        s_sd_reconcile_pending = true;
        return ESP_ERR_INVALID_STATE;
    }

    d1l_retained_blob_store_backend_state_t backend = {0};
    if (!d1l_retained_blob_store_backend_state(
            D1L_RETAINED_BLOB_STORE_READ_STATE, &backend) ||
        !backend.enabled ||
        backend.generation != s_accepted_sd_backend_generation) {
        /* The mutation is durable in the local journal. Reconciliation owns
         * any newly inserted/replaced SD generation. */
        return ESP_OK;
    }

    if (authority == D1L_READ_STATE_AUTHORITY_EXPLICIT_CLEAR) {
        ret = d1l_retained_blob_store_erase_sd_primary_guarded(
            D1L_RETAINED_BLOB_STORE_READ_STATE, D1L_READ_STATE_KEY,
            backend.generation);
    } else {
        ret = d1l_retained_blob_store_write_sd_primary_guarded(
            D1L_RETAINED_BLOB_STORE_READ_STATE, D1L_READ_STATE_KEY,
            &s_state, sizeof(s_state), backend.generation);
    }
    if (ret != ESP_OK ||
        !sd_backend_generation_matches(backend.generation) ||
        s_persistence_revision != expected_revision) {
        if (ret != ESP_OK) {
            note_persistence_failure(ret);
        }
        return ESP_OK;
    }

    ret = d1l_retained_blob_store_erase_nvs_fallback(
        D1L_RETAINED_BLOB_STORE_READ_STATE, D1L_READ_STATE_KEY);
    if (ret != ESP_OK) {
        s_nvs_fallback_dirty = true;
        note_persistence_failure(ret);
        return ESP_OK;
    }
    if (!sd_backend_generation_matches(backend.generation) ||
        s_persistence_revision != expected_revision) {
        s_nvs_fallback_dirty = true;
        s_sd_reconcile_pending = true;
        const esp_err_t journal_ret = write_fallback_journal();
        if (journal_ret != ESP_OK) {
            note_persistence_failure(journal_ret);
        }
        return ESP_OK;
    }
    s_authority = D1L_READ_STATE_AUTHORITY_NONE;
    s_sd_primary_dirty = false;
    s_sd_reconcile_pending = false;
    s_nvs_fallback_dirty = false;
    note_persistence_commit();
    return ESP_OK;
}

static void note_cursor_advance(void)
{
    if (s_state.mark_read_count < UINT32_MAX) {
        s_state.mark_read_count++;
    }
}

/* Callers hold s_persist_io_lock followed by s_store_lock. */
static esp_err_t init_locked(void)
{
    clear_ram();
    s_loaded = false;
    s_authority = D1L_READ_STATE_AUTHORITY_NONE;
    s_persistence_revision = 0U;
    s_persistence_commit_count = 0U;
    s_persistence_fail_count = 0U;
    s_persistence_last_error = ESP_OK;
    s_sd_primary_dirty = false;
    s_sd_reconcile_pending = false;
    s_nvs_fallback_dirty = false;

    d1l_retained_blob_store_backend_state_t backend = {0};
    if (!d1l_retained_blob_store_backend_state(
            D1L_RETAINED_BLOB_STORE_READ_STATE, &backend)) {
        note_persistence_failure(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    s_accepted_sd_backend_generation = backend.generation;

    d1l_read_state_v2_blob_t fallback = {0};
    bool fallback_found = false;
    bool fallback_migrated = false;
    esp_err_t ret = read_fallback_blob(
        &fallback, &fallback_found, &fallback_migrated);
    if (ret != ESP_OK) {
        note_persistence_failure(ret);
        return ret;
    }
    if (fallback_found) {
        s_state = fallback;
        s_authority = blob_is_empty(&fallback) ?
            D1L_READ_STATE_AUTHORITY_EXPLICIT_CLEAR :
            D1L_READ_STATE_AUTHORITY_LOCAL;
        s_sd_primary_dirty = true;
        s_sd_reconcile_pending = true;
        s_nvs_fallback_dirty = fallback_migrated;
    }

    s_loaded = true;
    if (!backend.enabled) {
        if (s_nvs_fallback_dirty) {
            ret = write_fallback_journal();
        }
        if (ret != ESP_OK) {
            s_loaded = false;
            note_persistence_failure(ret);
        }
        return ret;
    }

    /* Even a clean boot reads the exact current SD generation before Home can
     * consume cursors. A fallback journal is merged/cleared in that same
     * generation-fenced pass and retired only after SD is durable. */
    s_sd_reconcile_pending = true;
    ret = flush_loaded();
    if (ret != ESP_OK) {
        s_loaded = false;
        note_persistence_failure(ret);
    }
    return ret;
}

/* The I/O lock is held by the caller; this helper takes the store lock only. */
static esp_err_t ensure_loaded_io_locked(void)
{
    d1l_store_lock_take(&s_store_lock);
    const esp_err_t ret = s_loaded ? ESP_OK : init_locked();
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

static esp_err_t flush_io_locked(void)
{
    esp_err_t ret = ensure_loaded_io_locked();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_store_lock_take(&s_store_lock);
    ret = flush_loaded();
    if (ret != ESP_OK) {
        note_persistence_failure(ret);
    } else {
        s_persistence_last_error = ESP_OK;
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

esp_err_t d1l_read_state_init(void)
{
    d1l_store_lock_take(&s_persist_io_lock);
    d1l_store_lock_take(&s_store_lock);
    const esp_err_t ret = init_locked();
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    return ret;
}

esp_err_t d1l_read_state_clear(void)
{
    d1l_store_lock_take(&s_persist_io_lock);
    esp_err_t ret = flush_io_locked();
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }
    d1l_store_lock_take(&s_store_lock);
    d1l_read_state_runtime_snapshot_t previous = {0};
    capture_runtime(&previous);
    ret = reserve_persistence_revision();
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }
    clear_ram();
    ret = persist_mutation_or_rollback(
        &previous, D1L_READ_STATE_AUTHORITY_EXPLICIT_CLEAR);
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    return ret;
}

esp_err_t d1l_read_state_flush(void)
{
    d1l_store_lock_take(&s_persist_io_lock);
    const esp_err_t ret = flush_io_locked();
    d1l_store_lock_give(&s_persist_io_lock);
    return ret;
}

esp_err_t d1l_read_state_flush_if_due(void)
{
    /* Read-state blobs are small and every user mutation is synchronous.
     * "If due" therefore differs only in call-site intent, not durability. */
    d1l_store_lock_take(&s_persist_io_lock);
    const esp_err_t ret = flush_io_locked();
    d1l_store_lock_give(&s_persist_io_lock);
    return ret;
}

static int find_dm_cursor(const char *fingerprint)
{
    return find_cursor_in_blob(&s_state, fingerprint);
}

static uint32_t dm_thread_read_seq_for_blob(
    const d1l_read_state_v2_blob_t *blob, const char *fingerprint)
{
    const int idx = find_cursor_in_blob(blob, fingerprint);
    const uint32_t thread_seq =
        idx >= 0 ? blob->dm_cursors[idx].last_read_seq : 0U;
    const uint32_t read_seq = thread_seq > blob->last_dm_read_seq ?
        thread_seq : blob->last_dm_read_seq;
    const d1l_dm_store_stats_t dm_stats = d1l_dm_store_stats();
    /* Retained stores restart sequence numbering only after an explicit clear.
     * A cursor outside the new generation must not suppress new seq=1 rows. */
    return read_seq >= dm_stats.next_seq ? 0U : read_seq;
}

static esp_err_t upsert_dm_cursor(const char *fingerprint, uint32_t last_read_seq)
{
    if (!fingerprint || fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int idx = find_dm_cursor(fingerprint);
    if (idx < 0) {
        if (s_state.dm_cursor_count < D1L_READ_STATE_DM_THREAD_CAPACITY) {
            idx = (int)s_state.dm_cursor_count++;
        } else {
            idx = 0;
            for (uint32_t i = 1; i < D1L_READ_STATE_DM_THREAD_CAPACITY; ++i) {
                if (s_state.dm_cursors[i].last_read_seq < s_state.dm_cursors[idx].last_read_seq) {
                    idx = (int)i;
                }
            }
        }
    }

    memset(&s_state.dm_cursors[idx], 0, sizeof(s_state.dm_cursors[idx]));
    strncpy(s_state.dm_cursors[idx].fingerprint, fingerprint,
            sizeof(s_state.dm_cursors[idx].fingerprint) - 1U);
    s_state.dm_cursors[idx].last_read_seq = last_read_seq;
    return ESP_OK;
}

static size_t build_dm_thread_stats(
    const d1l_read_state_v2_blob_t *state,
    d1l_read_state_dm_thread_t *out_threads, size_t max_threads)
{
    if (!state || !out_threads || max_threads == 0) {
        return 0;
    }

    memset(out_threads, 0, sizeof(out_threads[0]) * max_threads);
    size_t thread_count = 0;
    const size_t copied = d1l_dm_store_copy_recent(
        s_dm_scratch, D1L_READ_STATE_VISIBLE_DM_CAPACITY);
    for (size_t i = 0; i < copied; ++i) {
        const d1l_dm_entry_t *entry = &s_dm_scratch[i];
        if (entry->direction[0] != 'r' || entry->contact_fingerprint[0] == '\0') {
            continue;
        }

        size_t thread_idx = thread_count;
        for (size_t j = 0; j < thread_count; ++j) {
            if (same_fingerprint(out_threads[j].fingerprint, entry->contact_fingerprint)) {
                thread_idx = j;
                break;
            }
        }
        if (thread_idx == thread_count) {
            if (thread_count >= max_threads) {
                continue;
            }
            d1l_read_state_dm_thread_t *thread = &out_threads[thread_idx];
            strncpy(thread->fingerprint, entry->contact_fingerprint,
                    sizeof(thread->fingerprint) - 1U);
            thread->last_read_seq = dm_thread_read_seq_for_blob(
                state, entry->contact_fingerprint);
            d1l_contact_entry_t contact = {0};
            thread->muted = d1l_contact_store_find_by_fingerprint(entry->contact_fingerprint,
                                                                   &contact) &&
                            contact.muted;
            thread_count++;
        }

        d1l_read_state_dm_thread_t *thread = &out_threads[thread_idx];
        if (entry->seq > thread->newest_rx_seq) {
            thread->newest_rx_seq = entry->seq;
        }
        if (entry->seq > thread->last_read_seq) {
            thread->unread_count++;
        }
    }
    return thread_count;
}

d1l_read_state_stats_t d1l_read_state_stats(void)
{
    d1l_retained_blob_store_backend_state_t backend = {0};
    const bool backend_known = d1l_retained_blob_store_backend_state(
        D1L_RETAINED_BLOB_STORE_READ_STATE, &backend);

    d1l_store_lock_take(&s_store_lock);
    const d1l_read_state_v2_blob_t state = s_state;
    const bool loaded = s_loaded;
    const d1l_read_state_authority_t authority = s_authority;
    const uint32_t accepted_generation =
        s_accepted_sd_backend_generation;
    const uint64_t persistence_revision = s_persistence_revision;
    const uint32_t persistence_commit_count =
        s_persistence_commit_count;
    const uint32_t persistence_fail_count = s_persistence_fail_count;
    const esp_err_t persistence_last_error = s_persistence_last_error;
    const bool sd_primary_dirty = s_sd_primary_dirty;
    const bool sd_reconcile_pending = s_sd_reconcile_pending;
    const bool nvs_fallback_dirty = s_nvs_fallback_dirty;
    d1l_store_lock_give(&s_store_lock);

    const bool backend_generation_changed =
        loaded && backend_known && backend.enabled &&
        backend.generation != accepted_generation;
    const bool projected_reconcile_pending =
        backend_known && backend.enabled &&
        (sd_reconcile_pending || backend_generation_changed);

    d1l_read_state_stats_t stats = {
        .last_public_read_seq = state.last_public_read_seq,
        .last_dm_read_seq = state.last_dm_read_seq,
        .mark_read_count = state.mark_read_count,
        .persisted_dm_cursor_count =
            loaded ? state.dm_cursor_count : 0U,
        .persisted_dm_cursor_capacity =
            D1L_READ_STATE_DM_THREAD_CAPACITY,
        .accepted_sd_backend_generation = accepted_generation,
        .sd_backend_generation = backend.generation,
        .persistence_revision = persistence_revision,
        .persistence_commit_count = persistence_commit_count,
        .persistence_fail_count = persistence_fail_count,
        .persistence_last_error = persistence_last_error,
        .loaded = loaded,
        .persistence_dirty =
            nvs_fallback_dirty ||
            (backend.enabled &&
             (sd_primary_dirty || projected_reconcile_pending)),
        .sd_primary_required = backend_known && backend.enabled,
        .sd_primary_dirty = sd_primary_dirty,
        .sd_primary_reconcile_pending = projected_reconcile_pending,
        .nvs_fallback_dirty = nvs_fallback_dirty,
        .clear_tombstone_pending =
            authority == D1L_READ_STATE_AUTHORITY_EXPLICIT_CLEAR,
    };

    d1l_store_lock_take(&s_projection_lock);
    d1l_message_store_stats_t message_stats = d1l_message_store_stats();
    d1l_dm_store_stats_t dm_stats = d1l_dm_store_stats();
    if (stats.last_public_read_seq >= message_stats.next_seq) {
        stats.last_public_read_seq = 0;
    }
    if (stats.last_dm_read_seq >= dm_stats.next_seq) {
        stats.last_dm_read_seq = 0;
    }

    size_t copied = d1l_message_store_copy_recent(s_message_scratch, D1L_MESSAGE_STORE_CAPACITY);
    for (size_t i = 0; i < copied; ++i) {
        const d1l_message_entry_t *entry = &s_message_scratch[i];
        if (entry->direction[0] != 'r') {
            continue;
        }
        if (entry->seq > stats.newest_public_rx_seq) {
            stats.newest_public_rx_seq = entry->seq;
        }
        if (entry->seq > stats.last_public_read_seq) {
            stats.public_unread_count++;
        }
    }

    stats.dm_thread_count = (uint32_t)build_dm_thread_stats(
        &state, s_thread_scratch, D1L_READ_STATE_VISIBLE_DM_CAPACITY);
    for (uint32_t i = 0; i < stats.dm_thread_count; ++i) {
        const d1l_read_state_dm_thread_t *thread = &s_thread_scratch[i];
        if (thread->newest_rx_seq > stats.newest_dm_rx_seq) {
            stats.newest_dm_rx_seq = thread->newest_rx_seq;
        }
        if (thread->muted) {
            stats.muted_dm_unread_count += thread->unread_count;
        } else {
            stats.dm_unread_count += thread->unread_count;
        }
    }
    d1l_store_lock_give(&s_projection_lock);

    return stats;
}

esp_err_t d1l_read_state_mark_public_read(void)
{
    d1l_store_lock_take(&s_persist_io_lock);
    esp_err_t ret = flush_io_locked();
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }

    const d1l_read_state_stats_t stats = d1l_read_state_stats();
    if (stats.newest_public_rx_seq <= stats.last_public_read_seq) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }
    d1l_store_lock_take(&s_store_lock);
    if (s_persistence_revision != stats.persistence_revision) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    d1l_read_state_runtime_snapshot_t previous = {0};
    capture_runtime(&previous);
    ret = reserve_persistence_revision();
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }
    s_state.last_public_read_seq = stats.newest_public_rx_seq;
    note_cursor_advance();
    ret = persist_mutation_or_rollback(
        &previous, D1L_READ_STATE_AUTHORITY_LOCAL);
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    return ret;
}

esp_err_t d1l_read_state_mark_dm_read(void)
{
    d1l_store_lock_take(&s_persist_io_lock);
    esp_err_t ret = flush_io_locked();
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }

    const d1l_read_state_stats_t stats = d1l_read_state_stats();
    if (stats.newest_dm_rx_seq <= stats.last_dm_read_seq) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }
    d1l_store_lock_take(&s_store_lock);
    if (s_persistence_revision != stats.persistence_revision) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    d1l_read_state_runtime_snapshot_t previous = {0};
    capture_runtime(&previous);
    ret = reserve_persistence_revision();
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }
    s_state.last_dm_read_seq = stats.newest_dm_rx_seq;
    note_cursor_advance();
    ret = persist_mutation_or_rollback(
        &previous, D1L_READ_STATE_AUTHORITY_LOCAL);
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    return ret;
}

esp_err_t d1l_read_state_mark_dm_thread_read(const char *fingerprint)
{
    if (!fingerprint || fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_store_lock_take(&s_persist_io_lock);
    esp_err_t ret = flush_io_locked();
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }

    uint32_t newest_rx_seq = 0;
    bool found_thread = false;
    d1l_store_lock_take(&s_projection_lock);
    const size_t copied = d1l_dm_store_copy_recent(
        s_dm_scratch, D1L_READ_STATE_VISIBLE_DM_CAPACITY);
    for (size_t i = 0; i < copied; ++i) {
        const d1l_dm_entry_t *entry = &s_dm_scratch[i];
        if (!same_fingerprint(entry->contact_fingerprint, fingerprint)) {
            continue;
        }
        found_thread = true;
        if (entry->direction[0] == 'r' && entry->seq > newest_rx_seq) {
            newest_rx_seq = entry->seq;
        }
    }
    d1l_store_lock_give(&s_projection_lock);
    if (!found_thread) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_NOT_FOUND;
    }

    /* Opening or refreshing an already-read thread is a read-only operation.
     * Only an actual advance of this exact fingerprint's cursor is persisted. */
    d1l_store_lock_take(&s_store_lock);
    if (newest_rx_seq <=
        dm_thread_read_seq_for_blob(&s_state, fingerprint)) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }

    d1l_read_state_runtime_snapshot_t previous = {0};
    capture_runtime(&previous);
    ret = reserve_persistence_revision();
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }
    ret = upsert_dm_cursor(fingerprint, newest_rx_seq);
    if (ret != ESP_OK) {
        restore_runtime(&previous);
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }
    note_cursor_advance();
    ret = persist_mutation_or_rollback(
        &previous, D1L_READ_STATE_AUTHORITY_LOCAL);
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    return ret;
}

esp_err_t d1l_read_state_mark_all_read(void)
{
    d1l_store_lock_take(&s_persist_io_lock);
    esp_err_t ret = flush_io_locked();
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }

    const d1l_read_state_stats_t stats = d1l_read_state_stats();
    const bool public_advance =
        stats.newest_public_rx_seq > stats.last_public_read_seq;
    const bool dm_advance = stats.newest_dm_rx_seq > stats.last_dm_read_seq;
    if (!public_advance && !dm_advance) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }
    d1l_store_lock_take(&s_store_lock);
    if (s_persistence_revision != stats.persistence_revision) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    d1l_read_state_runtime_snapshot_t previous = {0};
    capture_runtime(&previous);
    ret = reserve_persistence_revision();
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }
    if (public_advance) {
        s_state.last_public_read_seq = stats.newest_public_rx_seq;
    }
    if (dm_advance) {
        s_state.last_dm_read_seq = stats.newest_dm_rx_seq;
    }
    note_cursor_advance();
    ret = persist_mutation_or_rollback(
        &previous, D1L_READ_STATE_AUTHORITY_LOCAL);
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    return ret;
}

bool d1l_read_state_dm_entry_is_unread(const d1l_dm_entry_t *entry)
{
    if (!entry || entry->direction[0] != 'r') {
        return false;
    }
    d1l_store_lock_take(&s_store_lock);
    const d1l_read_state_v2_blob_t state = s_state;
    d1l_store_lock_give(&s_store_lock);
    return entry->seq >
        dm_thread_read_seq_for_blob(&state, entry->contact_fingerprint);
}

size_t d1l_read_state_copy_dm_threads(d1l_read_state_dm_thread_t *out_threads,
                                      size_t max_threads)
{
    d1l_store_lock_take(&s_store_lock);
    const d1l_read_state_v2_blob_t state = s_state;
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_take(&s_projection_lock);
    const size_t copied = build_dm_thread_stats(
        &state, out_threads, max_threads);
    d1l_store_lock_give(&s_projection_lock);
    return copied;
}

size_t d1l_read_state_copy_persisted_dm_cursors(
    d1l_read_state_persisted_dm_cursor_t *out_cursors, size_t max_cursors,
    uint32_t *out_accepted_sd_backend_generation)
{
    d1l_store_lock_take(&s_store_lock);
    if (out_accepted_sd_backend_generation) {
        *out_accepted_sd_backend_generation =
            s_accepted_sd_backend_generation;
    }
    if (!s_loaded || !out_cursors || max_cursors == 0U) {
        d1l_store_lock_give(&s_store_lock);
        return 0U;
    }
    const size_t copied = s_state.dm_cursor_count < max_cursors ?
        s_state.dm_cursor_count : max_cursors;
    memcpy(out_cursors, s_state.dm_cursors,
           copied * sizeof(out_cursors[0]));
    d1l_store_lock_give(&s_store_lock);
    return copied;
}

#ifdef D1L_READ_STATE_TEST_HOOKS
void d1l_read_state_test_set_after_sd_read_hook(void (*hook)(void))
{
    d1l_store_lock_take(&s_store_lock);
    s_after_sd_read_hook = hook;
    d1l_store_lock_give(&s_store_lock);
}
#endif
