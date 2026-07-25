#include "read_state.h"

#include <stdbool.h>
#include <string.h>

#include "esp_attr.h"
#include "nvs.h"

#include "mesh/contact_store.h"
#include "mesh/dm_store.h"
#include "mesh/message_store.h"

#define D1L_READ_STATE_NAMESPACE "d1l_read"
#define D1L_READ_STATE_KEY "state"
#define D1L_READ_STATE_SCHEMA 2U
#define D1L_READ_STATE_SCHEMA_V1 1U
/* The durable DM ring can expose one additional volatile row while its
 * persistence retry is pending.  Read cursors and global counts must include
 * that visible tail even though only 16 durable rows/cursors are retained. */
#define D1L_READ_STATE_VISIBLE_DM_CAPACITY (D1L_DM_STORE_CAPACITY + 1U)

typedef struct {
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    uint32_t last_read_seq;
} d1l_read_state_dm_cursor_t;

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
    d1l_read_state_dm_cursor_t dm_cursors[D1L_READ_STATE_DM_THREAD_CAPACITY];
} d1l_read_state_v2_blob_t;

static d1l_read_state_v2_blob_t s_state;
static bool s_loaded;
static d1l_message_entry_t
    s_message_scratch[D1L_MESSAGE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static d1l_dm_entry_t
    s_dm_scratch[D1L_READ_STATE_VISIBLE_DM_CAPACITY] EXT_RAM_BSS_ATTR;
static d1l_read_state_dm_thread_t
    s_thread_scratch[D1L_READ_STATE_VISIBLE_DM_CAPACITY] EXT_RAM_BSS_ATTR;

static void clear_ram(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.schema = D1L_READ_STATE_SCHEMA;
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

static esp_err_t persist_state(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_READ_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    s_state.schema = D1L_READ_STATE_SCHEMA;
    if (s_state.dm_cursor_count > D1L_READ_STATE_DM_THREAD_CAPACITY) {
        s_state.dm_cursor_count = D1L_READ_STATE_DM_THREAD_CAPACITY;
    }
    ret = nvs_set_blob(handle, D1L_READ_STATE_KEY, &s_state, sizeof(s_state));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t persist_mutation_or_rollback(
    const d1l_read_state_v2_blob_t *previous)
{
    if (!previous) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = persist_state();
    if (ret != ESP_OK) {
        s_state = *previous;
    }
    return ret;
}

static void note_cursor_advance(void)
{
    if (s_state.mark_read_count < UINT32_MAX) {
        s_state.mark_read_count++;
    }
}

static esp_err_t ensure_loaded(void)
{
    if (s_loaded) {
        return ESP_OK;
    }
    return d1l_read_state_init();
}

esp_err_t d1l_read_state_init(void)
{
    clear_ram();
    bool migrated = false;
    bool erased = false;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_READ_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        s_loaded = false;
        return ret;
    }

    size_t len = 0;
    ret = nvs_get_blob(handle, D1L_READ_STATE_KEY, NULL, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        if (len == sizeof(d1l_read_state_v2_blob_t)) {
            d1l_read_state_v2_blob_t loaded = {0};
            ret = nvs_get_blob(handle, D1L_READ_STATE_KEY, &loaded, &len);
            if (ret == ESP_OK && blob_v2_is_valid(&loaded, len)) {
                s_state = loaded;
            } else if (ret == ESP_OK) {
                ret = ESP_ERR_INVALID_VERSION;
            }
        } else if (len == sizeof(d1l_read_state_v1_blob_t)) {
            d1l_read_state_v1_blob_t loaded = {0};
            ret = nvs_get_blob(handle, D1L_READ_STATE_KEY, &loaded, &len);
            if (ret == ESP_OK && blob_v1_is_valid(&loaded, len)) {
                s_state.last_public_read_seq = loaded.last_public_read_seq;
                s_state.last_dm_read_seq = loaded.last_dm_read_seq;
                s_state.mark_read_count = loaded.mark_read_count;
                migrated = true;
            } else if (ret == ESP_OK) {
                ret = ESP_ERR_INVALID_VERSION;
            }
        } else {
            ret = ESP_ERR_INVALID_SIZE;
        }

        if (ret != ESP_OK) {
            clear_ram();
            ret = nvs_erase_key(handle, D1L_READ_STATE_KEY);
            if (ret == ESP_ERR_NVS_NOT_FOUND) {
                ret = ESP_OK;
            }
            erased = (ret == ESP_OK);
        }
        if (ret == ESP_OK && erased) {
            ret = nvs_commit(handle);
        }
    }
    nvs_close(handle);
    s_loaded = (ret == ESP_OK);
    if (s_loaded && migrated) {
        ret = persist_state();
        s_loaded = (ret == ESP_OK);
    }
    return ret;
}

esp_err_t d1l_read_state_clear(void)
{
    const d1l_read_state_v2_blob_t previous = s_state;
    const bool previous_loaded = s_loaded;
    clear_ram();
    s_loaded = true;
    const esp_err_t ret = persist_mutation_or_rollback(&previous);
    if (ret != ESP_OK) {
        s_loaded = previous_loaded;
    }
    return ret;
}

static bool same_fingerprint(const char *lhs, const char *rhs)
{
    return lhs && rhs &&
           strncmp(lhs, rhs, D1L_NODE_FINGERPRINT_LEN) == 0;
}

static int find_dm_cursor(const char *fingerprint)
{
    if (!fingerprint || fingerprint[0] == '\0') {
        return -1;
    }
    for (uint32_t i = 0; i < s_state.dm_cursor_count; ++i) {
        if (same_fingerprint(s_state.dm_cursors[i].fingerprint, fingerprint)) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t dm_thread_read_seq(const char *fingerprint)
{
    const int idx = find_dm_cursor(fingerprint);
    const uint32_t thread_seq = idx >= 0 ? s_state.dm_cursors[idx].last_read_seq : 0;
    const uint32_t read_seq = thread_seq > s_state.last_dm_read_seq ?
        thread_seq : s_state.last_dm_read_seq;
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

static size_t build_dm_thread_stats(d1l_read_state_dm_thread_t *out_threads, size_t max_threads)
{
    if (!out_threads || max_threads == 0) {
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
            thread->last_read_seq = dm_thread_read_seq(entry->contact_fingerprint);
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
    if (!s_loaded) {
        (void)d1l_read_state_init();
    }

    d1l_read_state_stats_t stats = {
        .last_public_read_seq = s_state.last_public_read_seq,
        .last_dm_read_seq = s_state.last_dm_read_seq,
        .mark_read_count = s_state.mark_read_count,
    };

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
        s_thread_scratch, D1L_READ_STATE_VISIBLE_DM_CAPACITY);
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

    return stats;
}

esp_err_t d1l_read_state_mark_public_read(void)
{
    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_read_state_stats_t stats = d1l_read_state_stats();
    if (stats.newest_public_rx_seq <= stats.last_public_read_seq) {
        return ESP_OK;
    }
    const d1l_read_state_v2_blob_t previous = s_state;
    s_state.last_public_read_seq = stats.newest_public_rx_seq;
    note_cursor_advance();
    return persist_mutation_or_rollback(&previous);
}

esp_err_t d1l_read_state_mark_dm_read(void)
{
    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_read_state_stats_t stats = d1l_read_state_stats();
    if (stats.newest_dm_rx_seq <= stats.last_dm_read_seq) {
        return ESP_OK;
    }
    const d1l_read_state_v2_blob_t previous = s_state;
    s_state.last_dm_read_seq = stats.newest_dm_rx_seq;
    note_cursor_advance();
    return persist_mutation_or_rollback(&previous);
}

esp_err_t d1l_read_state_mark_dm_thread_read(const char *fingerprint)
{
    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }
    if (!fingerprint || fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t newest_rx_seq = 0;
    bool found_thread = false;
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
    if (!found_thread) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Opening or refreshing an already-read thread is a read-only operation.
     * Only an actual advance of this exact fingerprint's cursor is persisted. */
    if (newest_rx_seq <= dm_thread_read_seq(fingerprint)) {
        return ESP_OK;
    }

    const d1l_read_state_v2_blob_t previous = s_state;
    ret = upsert_dm_cursor(fingerprint, newest_rx_seq);
    if (ret != ESP_OK) {
        return ret;
    }
    note_cursor_advance();
    return persist_mutation_or_rollback(&previous);
}

esp_err_t d1l_read_state_mark_all_read(void)
{
    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_read_state_stats_t stats = d1l_read_state_stats();
    const bool public_advance =
        stats.newest_public_rx_seq > stats.last_public_read_seq;
    const bool dm_advance = stats.newest_dm_rx_seq > stats.last_dm_read_seq;
    if (!public_advance && !dm_advance) {
        return ESP_OK;
    }
    const d1l_read_state_v2_blob_t previous = s_state;
    if (public_advance) {
        s_state.last_public_read_seq = stats.newest_public_rx_seq;
    }
    if (dm_advance) {
        s_state.last_dm_read_seq = stats.newest_dm_rx_seq;
    }
    note_cursor_advance();
    return persist_mutation_or_rollback(&previous);
}

bool d1l_read_state_dm_entry_is_unread(const d1l_dm_entry_t *entry)
{
    if (!entry || entry->direction[0] != 'r') {
        return false;
    }
    if (!s_loaded) {
        (void)d1l_read_state_init();
    }
    return entry->seq > dm_thread_read_seq(entry->contact_fingerprint);
}

size_t d1l_read_state_copy_dm_threads(d1l_read_state_dm_thread_t *out_threads,
                                      size_t max_threads)
{
    if (!s_loaded) {
        (void)d1l_read_state_init();
    }
    return build_dm_thread_stats(out_threads, max_threads);
}
