#include "packet_log.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

#include "hal/rp2040_bridge.h"
#include "mesh/route_store_worker.h"
#include "mesh/store_lock.h"
#include "storage/factory_reset.h"
#include "storage/retained_blob_store.h"

#define D1L_PACKET_LOG_KEY "ring"
#define D1L_PACKET_LOG_SCHEMA_V2 2U
#define D1L_PACKET_LOG_SCHEMA 3U
#define D1L_PACKET_LOG_HISTORY_MAGIC 0x314B5044UL
#define D1L_PACKET_LOG_HISTORY_SCHEMA 1U
#define D1L_PACKET_LOG_HISTORY_PATH_FMT "stores/packet_log/segments/s%02lu.bin"
#define D1L_PACKET_LOG_HISTORY_WRITE_TIMEOUT_MS 750U
#define D1L_PACKET_LOG_HISTORY_MAX_CONSECUTIVE_MISSES 4U

_Static_assert((D1L_PACKET_LOG_SD_CAPACITY % D1L_PACKET_LOG_SD_SEGMENT_COUNT) == 0,
               "packet SD history capacity must divide evenly into segments");

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    d1l_packet_log_entry_t entries[D1L_PACKET_LOG_NVS_FALLBACK_CAPACITY];
} d1l_packet_log_fallback_blob_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    d1l_packet_log_entry_t entries[D1L_PACKET_LOG_RAM_CAPACITY];
} d1l_packet_log_primary_blob_t;

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t entry_len;
    uint32_t checksum;
    d1l_packet_log_entry_t entry;
} d1l_packet_log_history_record_t;

_Static_assert(sizeof(d1l_packet_log_history_record_t) <= D1L_RP2040_FILE_CHUNK_MAX,
               "packet SD history record must fit in one RP2040 file chunk");

static d1l_packet_log_entry_t s_entries[D1L_PACKET_LOG_CAPACITY] EXT_RAM_BSS_ATTR;
static size_t s_head;
static size_t s_count;
static uint32_t s_next_seq = 1;
static uint32_t s_total_written;
static uint32_t s_dropped_oldest;
static uint32_t s_sd_dirty_count;
static uint32_t s_last_sd_flush_ms;
static uint32_t s_sd_history_records;
static uint32_t s_sd_history_failed_writes;
static uint32_t s_journal_next_seq = 1U;
static uint32_t s_last_sd_backend_generation;
static uint32_t s_boot_next_seq = 1U;
static uint32_t s_mutations_since_init;
static uint32_t s_persistence_commit_count;
static uint32_t s_persistence_fail_count;
static uint32_t s_sd_reconcile_count;
static uint32_t s_sd_reconcile_fail_count;
static esp_err_t s_sd_reconcile_last_error = ESP_OK;
static uint32_t s_sd_primary_commit_count;
static uint32_t s_sd_primary_fail_count;
static esp_err_t s_sd_primary_last_error = ESP_OK;
static uint32_t s_nvs_fallback_commit_count;
static uint32_t s_nvs_fallback_fail_count;
static esp_err_t s_nvs_fallback_last_error = ESP_OK;
static uint32_t s_journal_commit_count;
static uint32_t s_journal_fail_count;
static esp_err_t s_journal_last_error = ESP_OK;
static uint64_t s_revision;
static bool s_sd_primary_dirty;
static bool s_sd_reconcile_pending;
static bool s_nvs_fallback_dirty;
static bool s_journal_dirty;
static bool s_mutated_since_init;
static bool s_clear_in_progress;
static d1l_packet_log_entry_t s_volatile_entry;
static bool s_volatile_valid;
static bool s_loaded;
static d1l_packet_log_primary_blob_t s_primary_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_packet_log_primary_blob_t s_sd_primary_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_packet_log_primary_blob_t s_local_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_packet_log_entry_t
    s_merge_entries[D1L_PACKET_LOG_RAM_CAPACITY * 2U] EXT_RAM_BSS_ATTR;
static d1l_packet_log_fallback_blob_t s_fallback_blob_scratch;
static d1l_store_lock_t s_store_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_persist_io_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_append_clear_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_init_recovery_lock = D1L_STORE_LOCK_INITIALIZER;

static bool packet_entries_equal(const d1l_packet_log_entry_t *left,
                                 const d1l_packet_log_entry_t *right);

static bool packet_backend_state(d1l_retained_blob_store_backend_state_t *out_state)
{
    if (!out_state) {
        return false;
    }
    return d1l_retained_blob_store_backend_state(
        D1L_RETAINED_BLOB_STORE_PACKET_LOG, out_state);
}

static bool packet_backend_generation_matches(uint32_t expected_generation)
{
    d1l_retained_blob_store_backend_state_t state = {0};
    return packet_backend_state(&state) && state.enabled &&
           state.generation == expected_generation;
}

static esp_err_t packet_read_sd_primary(void *dst, size_t *len_inout)
{
    return d1l_retained_blob_store_read_sd_primary(
        D1L_RETAINED_BLOB_STORE_PACKET_LOG, D1L_PACKET_LOG_KEY,
        dst, len_inout);
}

static esp_err_t packet_read_nvs_fallback(void *dst, size_t *len_inout)
{
    return d1l_retained_blob_store_read_nvs_fallback(
        D1L_RETAINED_BLOB_STORE_PACKET_LOG, D1L_PACKET_LOG_KEY,
        dst, len_inout);
}

static esp_err_t packet_write_sd_primary_guarded(
    const void *src, size_t len, uint32_t expected_generation)
{
    return d1l_retained_blob_store_write_sd_primary_guarded(
        D1L_RETAINED_BLOB_STORE_PACKET_LOG, D1L_PACKET_LOG_KEY,
        src, len, expected_generation);
}

static esp_err_t packet_write_nvs_fallback(const void *src, size_t len)
{
    return d1l_retained_blob_store_write_nvs_fallback(
        D1L_RETAINED_BLOB_STORE_PACKET_LOG, D1L_PACKET_LOG_KEY,
        src, len);
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

static void raw_to_hex_preview(char *dest, size_t dest_size, const uint8_t *raw, size_t raw_len)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!dest || dest_size == 0) {
        return;
    }
    size_t out = 0;
    const size_t preview_len =
        raw_len < D1L_PACKET_LOG_RAW_PREVIEW_BYTES ? raw_len : D1L_PACKET_LOG_RAW_PREVIEW_BYTES;
    for (size_t i = 0; raw && i < preview_len && out + 2U < dest_size; ++i) {
        dest[out++] = hex[(raw[i] >> 4) & 0x0fU];
        dest[out++] = hex[raw[i] & 0x0fU];
    }
    dest[out] = '\0';
}

static int lower_ascii(int c)
{
    return isupper((unsigned char)c) ? tolower((unsigned char)c) : c;
}

static bool token_is_any(const char *token)
{
    return token == NULL || token[0] == '\0' ||
           strcmp(token, "any") == 0 || strcmp(token, "*") == 0;
}

static bool equals_ignore_case(const char *left, const char *right)
{
    if (!left || !right) {
        return false;
    }
    while (*left && *right) {
        if (lower_ascii(*left++) != lower_ascii(*right++)) {
            return false;
        }
    }
    return *left == '\0' && *right == '\0';
}

static bool contains_ignore_case(const char *haystack, const char *needle)
{
    if (token_is_any(needle)) {
        return true;
    }
    if (!haystack) {
        return false;
    }
    for (const char *h = haystack; *h; ++h) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b && lower_ascii(*a) == lower_ascii(*b)) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static bool packet_matches(const d1l_packet_log_entry_t *entry, const char *direction,
                           const char *kind, const char *search_text)
{
    if (!entry) {
        return false;
    }
    if (!token_is_any(direction) && !equals_ignore_case(entry->direction, direction)) {
        return false;
    }
    if (!token_is_any(kind)) {
        if (equals_ignore_case(kind, "text")) {
            if (!contains_ignore_case(entry->kind, "text")) {
                return false;
            }
        } else if (!equals_ignore_case(entry->kind, kind)) {
            return false;
        }
    }
    if (!token_is_any(search_text) &&
        !contains_ignore_case(entry->kind, search_text) &&
        !contains_ignore_case(entry->direction, search_text) &&
        !contains_ignore_case(entry->note, search_text) &&
        !contains_ignore_case(entry->raw_hex, search_text)) {
        return false;
    }
    return true;
}

static uint32_t history_record_checksum(const d1l_packet_log_entry_t *entry)
{
    const uint8_t *data = (const uint8_t *)entry;
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; entry && i < sizeof(*entry); ++i) {
        hash ^= data[i];
        hash *= 16777619UL;
    }
    return hash;
}

static bool history_segment_path(uint32_t seq, char *dest, size_t dest_size)
{
    if (seq == 0 || !dest || dest_size == 0) {
        return false;
    }
    const uint32_t segment =
        ((seq - 1U) / D1L_PACKET_LOG_SD_SEGMENT_CAPACITY) % D1L_PACKET_LOG_SD_SEGMENT_COUNT;
    const int written = snprintf(dest, dest_size, D1L_PACKET_LOG_HISTORY_PATH_FMT,
                                 (unsigned long)segment);
    return written > 0 && (size_t)written < dest_size;
}

static void fill_history_record(d1l_packet_log_history_record_t *record,
                                const d1l_packet_log_entry_t *entry)
{
    memset(record, 0, sizeof(*record));
    record->magic = D1L_PACKET_LOG_HISTORY_MAGIC;
    record->schema = D1L_PACKET_LOG_HISTORY_SCHEMA;
    record->entry_len = sizeof(record->entry);
    record->entry = *entry;
    record->checksum = history_record_checksum(&record->entry);
}

static bool history_record_is_valid(const d1l_packet_log_history_record_t *record,
                                    uint32_t expected_seq)
{
    return record &&
           record->magic == D1L_PACKET_LOG_HISTORY_MAGIC &&
           record->schema == D1L_PACKET_LOG_HISTORY_SCHEMA &&
           record->entry_len == sizeof(record->entry) &&
           record->entry.seq == expected_seq &&
           record->checksum == history_record_checksum(&record->entry);
}

typedef enum {
    D1L_PACKET_JOURNAL_SLOT_EMPTY = 0,
    D1L_PACKET_JOURNAL_SLOT_VALID,
    D1L_PACKET_JOURNAL_SLOT_MALFORMED,
} d1l_packet_journal_slot_state_t;

/* Read-only lineage proof. Missing files and the bridge's exact zero-byte EOF
 * reply are the only empty states. Occupied or malformed slots are returned
 * to the caller without mutation so reconciliation can preserve them and
 * resume at a fresh segment boundary. Transport and media-generation errors
 * fail closed. */
static esp_err_t probe_sd_history_slot(
    uint32_t seq, uint32_t expected_generation,
    d1l_packet_journal_slot_state_t *out_state,
    d1l_packet_log_entry_t *out_entry)
{
    if (seq == 0U || !out_state) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_state = D1L_PACKET_JOURNAL_SLOT_MALFORMED;
    if (out_entry) {
        memset(out_entry, 0, sizeof(*out_entry));
    }
    bool lineage_ready = false;
    const esp_err_t lineage_ret =
        d1l_retained_blob_store_sd_media_lineage_ready(
            D1L_RETAINED_BLOB_STORE_PACKET_LOG, expected_generation,
            &lineage_ready);
    if (lineage_ret != ESP_OK || !lineage_ready) {
        return lineage_ret == ESP_OK ? ESP_ERR_INVALID_STATE : lineage_ret;
    }
    if (!packet_backend_generation_matches(expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (d1l_route_store_persistence_should_yield()) {
        return ESP_ERR_NOT_FINISHED;
    }

    char path[D1L_RP2040_FILE_PATH_MAX + 1U];
    if (!history_segment_path(seq, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t offset =
        ((seq - 1U) % D1L_PACKET_LOG_SD_SEGMENT_CAPACITY) *
        (uint32_t)sizeof(d1l_packet_log_history_record_t);
    d1l_packet_log_history_record_t record = {0};
    d1l_rp2040_file_result_t result = {0};
    const esp_err_t ret = d1l_rp2040_bridge_file_read(
        path, offset, (uint8_t *)&record, sizeof(record), &result,
        D1L_PACKET_LOG_HISTORY_WRITE_TIMEOUT_MS);
    if (!packet_backend_generation_matches(expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (d1l_route_store_persistence_should_yield()) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (ret == ESP_ERR_NOT_FOUND &&
        strcmp(result.err, "not_found") == 0) {
        *out_state = D1L_PACKET_JOURNAL_SLOT_EMPTY;
        return ESP_OK;
    }
    if (ret == ESP_ERR_INVALID_SIZE && strcmp(result.err, "range") == 0) {
        /* The RP2040 reports range when this segment exists but the fixed
         * sequence slot lies beyond its current EOF. That is distinct from a
         * missing segment and must not be treated as a writable empty slot.
         * Preserve the short segment as malformed lineage so reconciliation
         * resumes only at a fresh segment boundary. */
        *out_state = D1L_PACKET_JOURNAL_SLOT_MALFORMED;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    const bool unwritten_eof = result.length == 0U && result.eof &&
                               result.offset == offset;
    if (unwritten_eof) {
        *out_state = D1L_PACKET_JOURNAL_SLOT_EMPTY;
        return ESP_OK;
    }
    if (result.length == sizeof(record) &&
        history_record_is_valid(&record, seq)) {
        *out_state = D1L_PACKET_JOURNAL_SLOT_VALID;
        if (out_entry) {
            *out_entry = record.entry;
        }
        return ESP_OK;
    }
    *out_state = D1L_PACKET_JOURNAL_SLOT_MALFORMED;
    return ESP_OK;
}

static bool read_sd_history_entry(uint32_t seq, d1l_packet_log_entry_t *out_entry)
{
    if (seq == 0 || !out_entry ||
        !d1l_retained_blob_store_uses_sd(D1L_RETAINED_BLOB_STORE_PACKET_LOG)) {
        return false;
    }
    d1l_retained_blob_store_backend_state_t backend = {0};
    if (!d1l_retained_blob_store_backend_state(
            D1L_RETAINED_BLOB_STORE_PACKET_LOG, &backend) ||
        !backend.enabled) {
        return false;
    }
    bool lineage_ready = false;
    if (d1l_retained_blob_store_sd_media_lineage_ready(
            D1L_RETAINED_BLOB_STORE_PACKET_LOG, backend.generation,
            &lineage_ready) != ESP_OK || !lineage_ready) {
        return false;
    }

    char path[D1L_RP2040_FILE_PATH_MAX + 1U];
    if (!history_segment_path(seq, path, sizeof(path))) {
        return false;
    }
    const uint32_t offset =
        ((seq - 1U) % D1L_PACKET_LOG_SD_SEGMENT_CAPACITY) *
        (uint32_t)sizeof(d1l_packet_log_history_record_t);
    d1l_packet_log_history_record_t record = {0};
    d1l_rp2040_file_result_t result = {0};
    const esp_err_t ret = d1l_rp2040_bridge_file_read(path, offset,
                                                       (uint8_t *)&record,
                                                       sizeof(record), &result,
                                                       D1L_PACKET_LOG_HISTORY_WRITE_TIMEOUT_MS);
    d1l_retained_blob_store_backend_state_t final_backend = {0};
    if (!d1l_retained_blob_store_backend_state(
            D1L_RETAINED_BLOB_STORE_PACKET_LOG, &final_backend) ||
        !final_backend.enabled ||
        final_backend.generation != backend.generation || ret != ESP_OK ||
        result.length != sizeof(record) ||
        !history_record_is_valid(&record, seq)) {
        return false;
    }
    *out_entry = record.entry;
    return true;
}

static void restore_sd_history_count_from_total(void)
{
    s_sd_history_records = s_total_written < D1L_PACKET_LOG_SD_CAPACITY ?
                           s_total_written : D1L_PACKET_LOG_SD_CAPACITY;
    s_sd_history_failed_writes = 0;
}

static esp_err_t append_sd_history_for_generation(
    const d1l_packet_log_entry_t *entry, uint32_t expected_generation)
{
    if (!entry) {
        return ESP_ERR_INVALID_ARG;
    }
    bool lineage_ready = false;
    const esp_err_t lineage_ret =
        d1l_retained_blob_store_sd_media_lineage_ready(
            D1L_RETAINED_BLOB_STORE_PACKET_LOG, expected_generation,
            &lineage_ready);
    if (lineage_ret != ESP_OK || !lineage_ready) {
        return lineage_ret == ESP_OK ? ESP_ERR_INVALID_STATE : lineage_ret;
    }
    /* A journal append/truncate is destructive in place, so it is forbidden
     * until the compact primary for this exact mounted generation has been
     * reconciled. Recheck immediately before issuing the bridge mutation. */
    if (!packet_backend_generation_matches(expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (d1l_route_store_persistence_should_yield()) {
        return ESP_ERR_NOT_FINISHED;
    }

    char path[D1L_RP2040_FILE_PATH_MAX + 1U];
    if (!history_segment_path(entry->seq, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }

    d1l_packet_log_history_record_t record;
    fill_history_record(&record, entry);
    const uint32_t offset =
        ((entry->seq - 1U) % D1L_PACKET_LOG_SD_SEGMENT_CAPACITY) *
        (uint32_t)sizeof(d1l_packet_log_history_record_t);
    const bool segment_start =
        ((entry->seq - 1U) % D1L_PACKET_LOG_SD_SEGMENT_CAPACITY) == 0;

    /* A journal record can be durable before the compact primary if power is
     * lost between those commits. Probe its deterministic slot first so the
     * retry is idempotent instead of appending a duplicate at EOF. */
    d1l_packet_log_history_record_t existing = {0};
    d1l_rp2040_file_result_t read_result = {0};
    const esp_err_t read_ret = d1l_rp2040_bridge_file_read(
        path, offset, (uint8_t *)&existing, sizeof(existing), &read_result,
        D1L_PACKET_LOG_HISTORY_WRITE_TIMEOUT_MS);
    bool truncate = segment_start;
    if (read_ret == ESP_OK) {
        const bool unwritten_eof = read_result.length == 0U &&
                                   read_result.eof &&
                                   read_result.offset == offset;
        if (unwritten_eof) {
            /* The RP2040 bridge reports offset == file_size as a successful
             * zero-byte EOF read. That is the normal next fixed journal slot,
             * not corruption. */
        } else if (read_result.length != sizeof(existing)) {
            return ESP_ERR_INVALID_STATE;
        } else if (history_record_is_valid(&existing, entry->seq)) {
            if (packet_entries_equal(&existing.entry, entry)) {
                if (!segment_start) {
                    return packet_backend_generation_matches(expected_generation) ?
                           ESP_OK : ESP_ERR_INVALID_STATE;
                }
                /* Even a byte-equal boundary record must be rewritten with
                 * truncate=true so stale later slots from the prior segment
                 * cycle cannot survive behind it. */
            }
            if (!packet_entries_equal(&existing.entry, entry) &&
                !segment_start) {
                return ESP_ERR_INVALID_STATE;
            }
            /* A fresh segment-cycle boundary is the sole point where a valid
             * prior-cycle record with the same modular sequence slot may be
             * replaced. Preserve every such collision in interior slots. */
        } else if (!segment_start) {
            /* Only a fresh segment-cycle boundary may replace a prior-cycle
             * slot. A corrupt/mismatched interior slot is preserved. */
            return ESP_ERR_INVALID_STATE;
        }
    } else if (read_ret != ESP_ERR_NOT_FOUND) {
        return read_ret;
    }

    if (!packet_backend_generation_matches(expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (d1l_route_store_persistence_should_yield()) {
        return ESP_ERR_NOT_FINISHED;
    }
    d1l_rp2040_file_result_t result = {0};
    const esp_err_t ret = d1l_rp2040_bridge_file_write(
        path, offset, (const uint8_t *)&record, sizeof(record), truncate,
        &result, D1L_PACKET_LOG_HISTORY_WRITE_TIMEOUT_MS);
    if (ret != ESP_OK || result.length != sizeof(record)) {
        return ret == ESP_OK ? ESP_FAIL : ret;
    }
    return ESP_OK;
}

static esp_err_t clear_sd_history_for_generation(uint32_t expected_generation)
{
    if (!packet_backend_generation_matches(expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }

    for (uint32_t segment = 0; segment < D1L_PACKET_LOG_SD_SEGMENT_COUNT; ++segment) {
        char path[D1L_RP2040_FILE_PATH_MAX + 1U];
        const int written = snprintf(path, sizeof(path), D1L_PACKET_LOG_HISTORY_PATH_FMT,
                                     (unsigned long)segment);
        if (written <= 0 || (size_t)written >= sizeof(path)) {
            return ESP_ERR_INVALID_SIZE;
        }
        /* The bridge API cannot make a delete conditional on media identity,
         * so check immediately around every command and stop on the first edge.
         * A generation-guarded delete backend would be required to close the
         * remaining inside-one-command physical replacement window. */
        if (!packet_backend_generation_matches(expected_generation)) {
            return ESP_ERR_INVALID_STATE;
        }
        d1l_rp2040_file_result_t result = {0};
        esp_err_t ret = d1l_rp2040_bridge_file_delete(path, &result,
                                                      D1L_PACKET_LOG_HISTORY_WRITE_TIMEOUT_MS);
        if (!packet_backend_generation_matches(expected_generation)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (ret == ESP_ERR_NOT_FOUND || strcmp(result.err, "not_found") == 0) {
            ret = ESP_OK;
        }
        if (ret != ESP_OK) {
            return ret;
        }
    }
    d1l_store_lock_take(&s_store_lock);
    s_sd_history_records = 0;
    s_sd_history_failed_writes = 0;
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

static void clear_ram(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_head = 0;
    s_count = 0;
    s_next_seq = 1;
    s_total_written = 0;
    s_dropped_oldest = 0;
    s_sd_dirty_count = 0;
    s_last_sd_flush_ms = 0;
    s_sd_history_records = 0;
    s_sd_history_failed_writes = 0;
    memset(&s_volatile_entry, 0, sizeof(s_volatile_entry));
    s_volatile_valid = false;
}

static void reset_persistence_state(uint32_t backend_generation,
                                    bool sd_enabled)
{
    s_journal_next_seq = 1U;
    s_last_sd_backend_generation = backend_generation;
    s_boot_next_seq = 1U;
    s_mutations_since_init = 0U;
    s_persistence_commit_count = 0U;
    s_persistence_fail_count = 0U;
    s_sd_reconcile_count = 0U;
    s_sd_reconcile_fail_count = 0U;
    s_sd_reconcile_last_error = ESP_OK;
    s_sd_primary_commit_count = 0U;
    s_sd_primary_fail_count = 0U;
    s_sd_primary_last_error = ESP_OK;
    s_nvs_fallback_commit_count = 0U;
    s_nvs_fallback_fail_count = 0U;
    s_nvs_fallback_last_error = ESP_OK;
    s_journal_commit_count = 0U;
    s_journal_fail_count = 0U;
    s_journal_last_error = ESP_OK;
    s_revision = 0U;
    s_sd_primary_dirty = false;
    s_sd_reconcile_pending = sd_enabled;
    s_nvs_fallback_dirty = false;
    s_journal_dirty = false;
    s_mutated_since_init = false;
    s_clear_in_progress = false;
}

static bool persistence_dirty_locked(bool sd_enabled)
{
    return s_nvs_fallback_dirty ||
           (sd_enabled && (s_sd_primary_dirty || s_sd_reconcile_pending ||
                           s_journal_dirty));
}

static void fill_entries(d1l_packet_log_entry_t *dest, size_t dest_capacity,
                         uint32_t *out_head, uint32_t *out_count)
{
    if (!dest || dest_capacity == 0 || !out_head || !out_count) {
        return;
    }
    memset(dest, 0, dest_capacity * sizeof(dest[0]));
    if (s_count == 0) {
        *out_head = 0;
        *out_count = 0;
        return;
    }

    size_t oldest = (s_head + D1L_PACKET_LOG_CAPACITY - s_count) % D1L_PACKET_LOG_CAPACITY;
    const size_t skip = s_count > dest_capacity ? s_count - dest_capacity : 0;
    size_t seen = 0;
    size_t copied = 0;
    for (size_t i = 0; i < s_count; ++i) {
        const d1l_packet_log_entry_t *entry =
            &s_entries[(oldest + i) % D1L_PACKET_LOG_CAPACITY];
        if (seen++ < skip) {
            continue;
        }
        if (copied < dest_capacity) {
            dest[copied++] = *entry;
        }
    }
    *out_head = (uint32_t)(copied % dest_capacity);
    *out_count = (uint32_t)copied;
}

static void fill_fallback_blob(d1l_packet_log_fallback_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->schema = D1L_PACKET_LOG_SCHEMA;
    blob->next_seq = s_next_seq;
    blob->total_written = s_total_written;
    blob->dropped_oldest = s_dropped_oldest;
    fill_entries(blob->entries, D1L_PACKET_LOG_NVS_FALLBACK_CAPACITY,
                 &blob->head, &blob->count);
}

static void fill_primary_blob(d1l_packet_log_primary_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->schema = D1L_PACKET_LOG_SCHEMA;
    blob->next_seq = s_next_seq;
    blob->total_written = s_total_written;
    blob->dropped_oldest = s_dropped_oldest;
    fill_entries(blob->entries, D1L_PACKET_LOG_RAM_CAPACITY,
                 &blob->head, &blob->count);
}

static esp_err_t persist_store(bool flush_primary, bool force);

static bool persisted_text_is_valid(const char *text, size_t capacity)
{
    if (!text || capacity == 0U || memchr(text, '\0', capacity) == NULL) {
        return false;
    }
    for (size_t i = 0U; i < capacity && text[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (c < 32U || c > 126U || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

static bool persisted_entry_is_valid(const d1l_packet_log_entry_t *entry,
                                     uint32_t next_seq)
{
    return entry && entry->seq > 0U && entry->seq < next_seq &&
           ((!entry->raw_truncated &&
             entry->raw_len <= D1L_PACKET_LOG_RAW_PREVIEW_BYTES) ||
            (entry->raw_truncated &&
             entry->raw_len > D1L_PACKET_LOG_RAW_PREVIEW_BYTES)) &&
           persisted_text_is_valid(entry->direction, sizeof(entry->direction)) &&
           persisted_text_is_valid(entry->kind, sizeof(entry->kind)) &&
           persisted_text_is_valid(entry->note, sizeof(entry->note)) &&
           persisted_text_is_valid(entry->raw_hex, sizeof(entry->raw_hex));
}

static bool persisted_entries_are_valid(const d1l_packet_log_entry_t *entries,
                                        size_t count, uint32_t next_seq)
{
    if (count > 0U && next_seq <= count) {
        return false;
    }
    const uint32_t first_seq = next_seq - (uint32_t)count;
    for (size_t i = 0U; i < count; ++i) {
        if (!persisted_entry_is_valid(&entries[i], next_seq) ||
            entries[i].seq != first_seq + (uint32_t)i) {
            return false;
        }
    }
    return true;
}

static bool persisted_lineage_is_valid(uint32_t next_seq,
                                       uint32_t total_written,
                                       uint32_t dropped_oldest,
                                       uint32_t head, uint32_t count,
                                       size_t capacity,
                                       const d1l_packet_log_entry_t *entries)
{
    if (next_seq == 0U || count > capacity || head >= capacity ||
        head != count % capacity || dropped_oldest > total_written ||
        (total_written == 0U) != (count == 0U) ||
        total_written < count || total_written == UINT32_MAX ||
        next_seq != total_written + 1U) {
        return false;
    }
    return persisted_entries_are_valid(entries, count, next_seq);
}

static bool fallback_blob_is_valid(const d1l_packet_log_fallback_blob_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_PACKET_LOG_SCHEMA &&
           persisted_lineage_is_valid(
               blob->next_seq, blob->total_written, blob->dropped_oldest,
               blob->head, blob->count,
               D1L_PACKET_LOG_NVS_FALLBACK_CAPACITY, blob->entries);
}

static bool fallback_blob_v2_is_valid(const d1l_packet_log_fallback_blob_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_PACKET_LOG_SCHEMA_V2 &&
           persisted_lineage_is_valid(
               blob->next_seq, blob->total_written, blob->dropped_oldest,
               blob->head, blob->count,
               D1L_PACKET_LOG_NVS_FALLBACK_CAPACITY, blob->entries);
}

static bool primary_blob_is_valid(const d1l_packet_log_primary_blob_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_PACKET_LOG_SCHEMA &&
           persisted_lineage_is_valid(
               blob->next_seq, blob->total_written, blob->dropped_oldest,
               blob->head, blob->count, D1L_PACKET_LOG_RAM_CAPACITY,
               blob->entries);
}

static void promote_v2_compact_primary(
    const d1l_packet_log_fallback_blob_t *legacy,
    d1l_packet_log_primary_blob_t *primary)
{
    memset(primary, 0, sizeof(*primary));
    primary->schema = D1L_PACKET_LOG_SCHEMA;
    primary->next_seq = legacy->next_seq;
    primary->total_written = legacy->total_written;
    primary->dropped_oldest = legacy->dropped_oldest;
    primary->count = legacy->count;
    primary->head = legacy->count % D1L_PACKET_LOG_RAM_CAPACITY;
    memcpy(primary->entries, legacy->entries,
           legacy->count * sizeof(legacy->entries[0]));
}

static void load_fallback_blob_into_ram(
    const d1l_packet_log_fallback_blob_t *blob)
{
    memset(s_entries, 0, sizeof(s_entries));
    memcpy(s_entries, blob->entries,
           blob->count * sizeof(blob->entries[0]));
    s_head = blob->count % D1L_PACKET_LOG_CAPACITY;
    s_count = blob->count;
    s_next_seq = blob->next_seq;
    s_total_written = blob->total_written;
    s_dropped_oldest = blob->dropped_oldest;
    s_sd_dirty_count = 0;
    s_sd_history_records = 0U;
    s_sd_history_failed_writes = 0U;
}

static void load_primary_blob_into_ram(
    const d1l_packet_log_primary_blob_t *blob)
{
    memset(s_entries, 0, sizeof(s_entries));
    memcpy(s_entries, blob->entries,
           blob->count * sizeof(blob->entries[0]));
    s_head = blob->count % D1L_PACKET_LOG_CAPACITY;
    s_count = blob->count;
    s_next_seq = blob->next_seq;
    s_total_written = blob->total_written;
    s_dropped_oldest = blob->dropped_oldest;
    s_sd_dirty_count = 0;
}

static bool packet_entries_equal(const d1l_packet_log_entry_t *left,
                                 const d1l_packet_log_entry_t *right)
{
    return left && right && left->seq == right->seq &&
           left->uptime_ms == right->uptime_ms &&
           strcmp(left->direction, right->direction) == 0 &&
           strcmp(left->kind, right->kind) == 0 &&
           left->rssi_dbm == right->rssi_dbm &&
           left->snr_tenths == right->snr_tenths &&
           left->path_hash_bytes == right->path_hash_bytes &&
           left->path_hops == right->path_hops &&
           left->payload_len == right->payload_len &&
           left->raw_len == right->raw_len &&
           left->raw_truncated == right->raw_truncated &&
           strcmp(left->note, right->note) == 0 &&
           strcmp(left->raw_hex, right->raw_hex) == 0;
}

static bool primary_blobs_equal(const d1l_packet_log_primary_blob_t *left,
                                const d1l_packet_log_primary_blob_t *right)
{
    if (!left || !right || left->next_seq != right->next_seq ||
        left->total_written != right->total_written ||
        left->dropped_oldest != right->dropped_oldest ||
        left->count != right->count) {
        return false;
    }
    for (size_t i = 0U; i < left->count; ++i) {
        if (!packet_entries_equal(&left->entries[i], &right->entries[i])) {
            return false;
        }
    }
    return true;
}

static void fill_primary_from_merge(d1l_packet_log_primary_blob_t *out,
                                    size_t merged_count,
                                    uint32_t next_seq,
                                    uint32_t total_written,
                                    uint32_t dropped_oldest)
{
    memset(out, 0, sizeof(*out));
    const size_t skip = merged_count > D1L_PACKET_LOG_RAM_CAPACITY ?
                        merged_count - D1L_PACKET_LOG_RAM_CAPACITY : 0U;
    out->schema = D1L_PACKET_LOG_SCHEMA;
    out->count = (uint32_t)(merged_count - skip);
    out->head = out->count % D1L_PACKET_LOG_RAM_CAPACITY;
    out->next_seq = next_seq;
    out->total_written = total_written;
    const uint32_t minimum_dropped =
        total_written > D1L_PACKET_LOG_RAM_CAPACITY ?
        total_written - D1L_PACKET_LOG_RAM_CAPACITY : 0U;
    out->dropped_oldest = dropped_oldest > minimum_dropped ?
                          dropped_oldest : minimum_dropped;
    if (out->count > 0U) {
        memcpy(out->entries, &s_merge_entries[skip],
               out->count * sizeof(out->entries[0]));
    }
}

static esp_err_t merge_same_lineage(
    const d1l_packet_log_primary_blob_t *local,
    const d1l_packet_log_primary_blob_t *sd,
    d1l_packet_log_primary_blob_t *out)
{
    size_t local_index = 0U;
    size_t sd_index = 0U;
    size_t merged_count = 0U;
    while (local_index < local->count || sd_index < sd->count) {
        const d1l_packet_log_entry_t *selected = NULL;
        if (sd_index >= sd->count ||
            (local_index < local->count &&
             local->entries[local_index].seq < sd->entries[sd_index].seq)) {
            selected = &local->entries[local_index++];
        } else if (local_index >= local->count ||
                   sd->entries[sd_index].seq < local->entries[local_index].seq) {
            selected = &sd->entries[sd_index++];
        } else {
            if (!packet_entries_equal(&local->entries[local_index],
                                      &sd->entries[sd_index])) {
                return ESP_ERR_INVALID_STATE;
            }
            selected = &local->entries[local_index++];
            sd_index++;
        }
        if (merged_count >= D1L_PACKET_LOG_RAM_CAPACITY * 2U) {
            return ESP_ERR_INVALID_SIZE;
        }
        s_merge_entries[merged_count++] = *selected;
    }

    const uint32_t next_seq = local->next_seq > sd->next_seq ?
                              local->next_seq : sd->next_seq;
    const uint32_t total_written = local->total_written > sd->total_written ?
                                   local->total_written : sd->total_written;
    const uint32_t dropped_oldest = local->dropped_oldest > sd->dropped_oldest ?
                                    local->dropped_oldest : sd->dropped_oldest;
    fill_primary_from_merge(out, merged_count, next_seq, total_written,
                            dropped_oldest);
    return primary_blob_is_valid(out, sizeof(*out)) ?
           ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool merge_contains_exact_entry(size_t count,
                                       const d1l_packet_log_entry_t *entry)
{
    for (size_t i = 0U; i < count; ++i) {
        if (packet_entries_equal(&s_merge_entries[i], entry)) {
            return true;
        }
    }
    return false;
}

static esp_err_t append_unique_merge_entry(
    size_t *count, const d1l_packet_log_entry_t *entry)
{
    if (merge_contains_exact_entry(*count, entry)) {
        return ESP_OK;
    }
    if (*count >= D1L_PACKET_LOG_RAM_CAPACITY * 2U) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_merge_entries[(*count)++] = *entry;
    return ESP_OK;
}

static esp_err_t fill_resequenced_divergent_primary(
    d1l_packet_log_primary_blob_t *out, size_t merged_count,
    uint32_t preferred_total, uint32_t dropped_oldest)
{
    uint32_t total_written = preferred_total;
    if (merged_count > total_written) {
        if (merged_count >= UINT32_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        total_written = (uint32_t)merged_count;
    }
    if (total_written == UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t next_seq = total_written + 1U;
    fill_primary_from_merge(out, merged_count, next_seq, total_written,
                            dropped_oldest);
    if (out->count > 0U) {
        const uint32_t first_seq = next_seq - out->count;
        for (uint32_t i = 0U; i < out->count; ++i) {
            out->entries[i].seq = first_seq + i;
        }
    }
    return primary_blob_is_valid(out, sizeof(*out)) ?
           ESP_OK : ESP_ERR_INVALID_STATE;
}

/* A valid compact SD primary and a valid device-local NVS/live tail may be
 * separated by records that were irretrievably dropped while SD was absent.
 * Preserve every available row that fits the bounded primary, remove only
 * byte-for-byte duplicates, and deterministically resequence the retained
 * order into one contiguous current lineage. Same-seq different payloads are
 * separate evidence and are therefore both retained. */
static esp_err_t merge_divergent_lineage(
    const d1l_packet_log_primary_blob_t *local,
    const d1l_packet_log_primary_blob_t *sd,
    d1l_packet_log_primary_blob_t *out)
{
    size_t merged_count = 0U;
    esp_err_t ret = ESP_OK;
    if (local->total_written > sd->total_written) {
        size_t local_index = 0U;
        size_t sd_index = 0U;
        while (local_index < local->count || sd_index < sd->count) {
            if (sd_index >= sd->count ||
                (local_index < local->count &&
                 local->entries[local_index].seq < sd->entries[sd_index].seq)) {
                ret = append_unique_merge_entry(
                    &merged_count, &local->entries[local_index++]);
            } else if (local_index >= local->count ||
                       sd->entries[sd_index].seq < local->entries[local_index].seq) {
                ret = append_unique_merge_entry(
                    &merged_count, &sd->entries[sd_index++]);
            } else {
                ret = append_unique_merge_entry(
                    &merged_count, &sd->entries[sd_index++]);
                if (ret == ESP_OK) {
                    ret = append_unique_merge_entry(
                        &merged_count, &local->entries[local_index++]);
                }
            }
            if (ret != ESP_OK) {
                return ret;
            }
        }
    } else {
        /* The removable primary is newer or tied: retain its order as the
         * base, then replay every distinct device-local/live row as overlay. */
        for (size_t i = 0U; i < sd->count; ++i) {
            ret = append_unique_merge_entry(&merged_count, &sd->entries[i]);
            if (ret != ESP_OK) {
                return ret;
            }
        }
        for (size_t i = 0U; i < local->count; ++i) {
            ret = append_unique_merge_entry(&merged_count, &local->entries[i]);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }

    const uint32_t preferred_total =
        local->total_written > sd->total_written ?
        local->total_written : sd->total_written;
    const uint32_t dropped_oldest =
        local->dropped_oldest > sd->dropped_oldest ?
        local->dropped_oldest : sd->dropped_oldest;
    return fill_resequenced_divergent_primary(
        out, merged_count, preferred_total, dropped_oldest);
}

static esp_err_t adopt_sd_with_live_overlay(
    const d1l_packet_log_primary_blob_t *local,
    const d1l_packet_log_primary_blob_t *sd,
    uint32_t boot_next_seq, uint32_t expected_mutations,
    d1l_packet_log_primary_blob_t *out,
    uint32_t *out_overlay_count)
{
    size_t merged_count = 0U;
    for (size_t i = 0U; i < sd->count; ++i) {
        s_merge_entries[merged_count++] = sd->entries[i];
    }

    uint32_t overlay_count = 0U;
    uint32_t next_seq = sd->next_seq;
    for (size_t i = 0U; i < local->count; ++i) {
        if (local->entries[i].seq < boot_next_seq) {
            continue;
        }
        if (next_seq == UINT32_MAX ||
            merged_count >= D1L_PACKET_LOG_RAM_CAPACITY * 2U) {
            return ESP_ERR_INVALID_SIZE;
        }
        d1l_packet_log_entry_t overlay = local->entries[i];
        overlay.seq = next_seq++;
        s_merge_entries[merged_count++] = overlay;
        overlay_count++;
    }
    if (overlay_count != expected_mutations) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t total_written = sd->total_written + overlay_count;
    if (total_written < sd->total_written) {
        return ESP_ERR_INVALID_SIZE;
    }
    fill_primary_from_merge(out, merged_count, next_seq, total_written,
                            sd->dropped_oldest);
    if (!primary_blob_is_valid(out, sizeof(*out))) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_overlay_count) {
        *out_overlay_count = overlay_count;
    }
    return ESP_OK;
}

static uint32_t next_journal_segment_start(uint32_t next_seq)
{
    if (next_seq == 0U) {
        return UINT32_MAX;
    }
    const uint32_t within = (next_seq - 1U) %
                            D1L_PACKET_LOG_SD_SEGMENT_CAPACITY;
    if (within == 0U) {
        return next_seq;
    }
    const uint32_t advance = D1L_PACKET_LOG_SD_SEGMENT_CAPACITY - within;
    return next_seq > UINT32_MAX - advance ? UINT32_MAX : next_seq + advance;
}

static esp_err_t note_reconcile_failure(esp_err_t failure)
{
    d1l_store_lock_take(&s_store_lock);
    s_sd_reconcile_pending = true;
    s_sd_reconcile_fail_count++;
    s_sd_reconcile_last_error = failure;
    s_sd_primary_last_error = failure;
    d1l_store_lock_give(&s_store_lock);
    return failure;
}

/* Read and compare the compact primary before any journal append/truncate or
 * primary replace on a newly observed backend generation. */
static esp_err_t reconcile_sd_primary(uint32_t expected_generation)
{
    if (!packet_backend_generation_matches(expected_generation)) {
        return note_reconcile_failure(ESP_ERR_INVALID_STATE);
    }

    bool lineage_ready = false;
    esp_err_t lineage_ret =
        d1l_retained_blob_store_sd_media_lineage_ready(
            D1L_RETAINED_BLOB_STORE_PACKET_LOG, expected_generation,
            &lineage_ready);
    if (lineage_ret != ESP_OK) {
        return note_reconcile_failure(lineage_ret);
    }
    if (!lineage_ready) {
        lineage_ret = clear_sd_history_for_generation(expected_generation);
        if (lineage_ret == ESP_OK) {
            lineage_ret =
                d1l_retained_blob_store_complete_packet_reset_lineage(
                    expected_generation);
        }
        if (lineage_ret != ESP_OK ||
            !packet_backend_generation_matches(expected_generation)) {
            return note_reconcile_failure(
                lineage_ret == ESP_OK ? ESP_ERR_INVALID_STATE : lineage_ret);
        }
    }

    memset(&s_sd_primary_blob_scratch, 0, sizeof(s_sd_primary_blob_scratch));
    size_t sd_len = sizeof(s_sd_primary_blob_scratch);
    const esp_err_t sd_ret = packet_read_sd_primary(
        &s_sd_primary_blob_scratch, &sd_len);
    if (sd_ret == ESP_ERR_NOT_FINISHED) {
        return sd_ret;
    }
    bool primary_found = false;
    bool legacy_v2_primary = false;
    if (sd_ret == ESP_OK &&
        primary_blob_is_valid(&s_sd_primary_blob_scratch, sd_len)) {
        primary_found = true;
    } else if (sd_ret == ESP_OK &&
               sd_len == sizeof(s_fallback_blob_scratch)) {
        /* Schema v2 used the same eight-entry compact blob for SD and NVS.
         * Accept only that exact legacy size/layout, then expand it in RAM so
         * the normal merge and guarded replacement can upgrade it without
         * discarding newer NVS or live entries. */
        memcpy(&s_fallback_blob_scratch, &s_sd_primary_blob_scratch,
               sizeof(s_fallback_blob_scratch));
        if (fallback_blob_v2_is_valid(&s_fallback_blob_scratch, sd_len)) {
            promote_v2_compact_primary(&s_fallback_blob_scratch,
                                       &s_sd_primary_blob_scratch);
            primary_found = true;
            legacy_v2_primary = true;
        } else {
            return note_reconcile_failure(ESP_ERR_INVALID_STATE);
        }
    } else if (sd_ret == ESP_OK) {
        return note_reconcile_failure(ESP_ERR_INVALID_STATE);
    }
    if (sd_ret != ESP_OK && sd_ret != ESP_ERR_NOT_FOUND) {
        return note_reconcile_failure(sd_ret);
    }

    bool journal_continuation_trusted = false;
    if (primary_found && s_sd_primary_blob_scratch.total_written > 0U) {
        d1l_packet_journal_slot_state_t tail_state;
        d1l_packet_log_entry_t journal_tail = {0};
        esp_err_t probe_ret = probe_sd_history_slot(
            s_sd_primary_blob_scratch.next_seq - 1U,
            expected_generation, &tail_state, &journal_tail);
        if (probe_ret == ESP_ERR_NOT_FINISHED) {
            return probe_ret;
        }
        if (probe_ret != ESP_OK) {
            return note_reconcile_failure(probe_ret);
        }
        const d1l_packet_log_entry_t *primary_tail =
            &s_sd_primary_blob_scratch.entries[
                s_sd_primary_blob_scratch.count - 1U];
        if (tail_state == D1L_PACKET_JOURNAL_SLOT_VALID &&
            packet_entries_equal(&journal_tail, primary_tail)) {
            d1l_packet_journal_slot_state_t next_state;
            probe_ret = probe_sd_history_slot(
                s_sd_primary_blob_scratch.next_seq,
                expected_generation, &next_state, NULL);
            if (probe_ret == ESP_ERR_NOT_FINISHED) {
                return probe_ret;
            }
            if (probe_ret != ESP_OK) {
                return note_reconcile_failure(probe_ret);
            }
            journal_continuation_trusted =
                next_state == D1L_PACKET_JOURNAL_SLOT_EMPTY;
        }
    }
    if (!packet_backend_generation_matches(expected_generation)) {
        return note_reconcile_failure(ESP_ERR_INVALID_STATE);
    }

    bool journal_lineage_untrusted =
        primary_found && s_sd_primary_blob_scratch.total_written > 0U &&
        !journal_continuation_trusted;
    d1l_store_lock_take(&s_store_lock);
    fill_primary_blob(&s_local_blob_scratch);
    d1l_packet_log_primary_blob_t *merged = &s_primary_blob_scratch;
    esp_err_t merge_ret = ESP_OK;
    uint32_t overlay_count = 0U;
    if (!primary_found) {
        *merged = s_local_blob_scratch;
    } else {
        const bool sd_newer =
            s_sd_primary_blob_scratch.total_written >
                s_local_blob_scratch.total_written ||
            (s_sd_primary_blob_scratch.total_written ==
                 s_local_blob_scratch.total_written &&
             s_sd_primary_blob_scratch.next_seq >
                 s_local_blob_scratch.next_seq);
        if (sd_newer && s_mutated_since_init) {
            merge_ret = adopt_sd_with_live_overlay(
                &s_local_blob_scratch, &s_sd_primary_blob_scratch,
                s_boot_next_seq, s_mutations_since_init, merged,
                &overlay_count);
        } else if (sd_newer) {
            *merged = s_sd_primary_blob_scratch;
        } else {
            merge_ret = merge_same_lineage(
                &s_local_blob_scratch, &s_sd_primary_blob_scratch, merged);
            if (merge_ret == ESP_ERR_INVALID_STATE) {
                merge_ret = merge_divergent_lineage(
                    &s_local_blob_scratch, &s_sd_primary_blob_scratch, merged);
                journal_lineage_untrusted = merge_ret == ESP_OK;
            }
        }
    }
    if (merge_ret != ESP_OK) {
        s_sd_reconcile_pending = true;
        s_sd_reconcile_fail_count++;
        s_sd_reconcile_last_error = merge_ret;
        s_sd_primary_last_error = merge_ret;
        d1l_store_lock_give(&s_store_lock);
        return merge_ret;
    }

    /* The card may have changed while valid histories were being compared.
     * Re-sample at the last possible point before importing any SD-derived
     * rows into RAM or making that import eligible for an NVS write. */
    if (!packet_backend_generation_matches(expected_generation)) {
        s_sd_reconcile_pending = true;
        s_sd_reconcile_fail_count++;
        s_sd_reconcile_last_error = ESP_ERR_INVALID_STATE;
        s_sd_primary_last_error = ESP_ERR_INVALID_STATE;
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const bool local_changed =
        !primary_blobs_equal(&s_local_blob_scratch, merged);
    const bool sd_primary_changed = primary_found &&
        !primary_blobs_equal(&s_sd_primary_blob_scratch, merged);
    if (legacy_v2_primary || sd_primary_changed || overlay_count > 0U) {
        journal_lineage_untrusted = true;
    }
    if (local_changed) {
        load_primary_blob_into_ram(merged);
        s_revision++;
        s_nvs_fallback_dirty = true;
    }
    s_sd_primary_dirty = primary_found ?
        (legacy_v2_primary ||
         !primary_blobs_equal(&s_sd_primary_blob_scratch, merged)) :
        merged->total_written > 0U;
    s_sd_reconcile_pending = false;
    s_sd_reconcile_count++;
    s_sd_reconcile_last_error = ESP_OK;
    s_sd_primary_last_error = ESP_OK;
    s_last_sd_backend_generation = expected_generation;

    if (primary_found && s_sd_primary_blob_scratch.total_written == 0U &&
        merged->total_written == 0U) {
        s_sd_history_records = 0U;
        s_journal_next_seq = 1U;
    } else if (primary_found && journal_continuation_trusted &&
               !journal_lineage_untrusted) {
        restore_sd_history_count_from_total();
        s_sd_history_records =
            s_sd_primary_blob_scratch.total_written < D1L_PACKET_LOG_SD_CAPACITY ?
            s_sd_primary_blob_scratch.total_written : D1L_PACKET_LOG_SD_CAPACITY;
        s_journal_next_seq = s_sd_primary_blob_scratch.next_seq;
    } else if (!primary_found && merged->total_written == 0U) {
        s_sd_history_records = 0U;
        s_journal_next_seq = 1U;
    } else {
        /* An absent/unproven tail cannot safely receive an EOF append at a
         * seq-derived offset. Resume only when a fresh segment can truncate. */
        s_sd_history_records = 0U;
        s_journal_next_seq = next_journal_segment_start(merged->next_seq);
    }
    s_journal_dirty = s_journal_next_seq < s_next_seq;
    if (overlay_count > 0U) {
        s_boot_next_seq = s_next_seq - overlay_count;
        s_mutations_since_init = overlay_count;
        s_mutated_since_init = true;
    }
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

static bool copy_ram_entry_by_seq_locked(uint32_t seq,
                                         d1l_packet_log_entry_t *out_entry)
{
    const size_t oldest = s_count == 0U ? 0U :
        (s_head + D1L_PACKET_LOG_CAPACITY - s_count) %
        D1L_PACKET_LOG_CAPACITY;
    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_packet_log_entry_t *entry =
            &s_entries[(oldest + i) % D1L_PACKET_LOG_CAPACITY];
        if (entry->seq == seq) {
            *out_entry = *entry;
            return true;
        }
    }
    return false;
}

static esp_err_t flush_journal_for_generation(uint32_t expected_generation,
                                              uint32_t snapshot_next_seq)
{
    while (true) {
        if (d1l_route_store_persistence_should_yield()) {
            return ESP_ERR_NOT_FINISHED;
        }
        d1l_packet_log_entry_t entry = {0};
        d1l_store_lock_take(&s_store_lock);
        const uint32_t seq = s_journal_next_seq;
        if (seq >= snapshot_next_seq) {
            s_journal_dirty = s_journal_next_seq < s_next_seq;
            d1l_store_lock_give(&s_store_lock);
            return ESP_OK;
        }
        const bool found = copy_ram_entry_by_seq_locked(seq, &entry);
        d1l_store_lock_give(&s_store_lock);
        if (!found) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!packet_backend_generation_matches(expected_generation)) {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t ret = append_sd_history_for_generation(
            &entry, expected_generation);
        if (ret != ESP_OK ||
            !packet_backend_generation_matches(expected_generation)) {
            return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
        }
        d1l_store_lock_take(&s_store_lock);
        if (s_journal_next_seq == seq) {
            s_journal_next_seq++;
            if (s_sd_history_records < D1L_PACKET_LOG_SD_CAPACITY) {
                s_sd_history_records++;
            }
            s_journal_commit_count++;
            s_journal_last_error = ESP_OK;
        }
        s_journal_dirty = s_journal_next_seq < s_next_seq;
        d1l_store_lock_give(&s_store_lock);
    }
}

static esp_err_t persist_store(bool flush_primary, bool force)
{
    d1l_store_lock_take(&s_persist_io_lock);
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    if (!packet_backend_state(&backend_state)) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }

    d1l_store_lock_take(&s_store_lock);
    if (backend_state.generation != s_last_sd_backend_generation) {
        s_last_sd_backend_generation = backend_state.generation;
        s_sd_reconcile_pending = backend_state.enabled;
    }
    const bool reconcile_attempt = backend_state.enabled &&
                                   s_sd_reconcile_pending;
    d1l_store_lock_give(&s_store_lock);

    esp_err_t first_error = ESP_OK;
    bool operation_attempted = false;
    if (reconcile_attempt) {
        operation_attempted = true;
        const esp_err_t ret = reconcile_sd_primary(backend_state.generation);
        if (ret == ESP_ERR_NOT_FINISHED) {
            d1l_store_lock_give(&s_persist_io_lock);
            return ret;
        }
        if (ret != ESP_OK) {
            first_error = ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    fill_primary_blob(&s_primary_blob_scratch);
    fill_fallback_blob(&s_fallback_blob_scratch);
    const uint64_t snapshot_revision = s_revision;
    const uint32_t snapshot_next_seq = s_next_seq;
    const bool sd_ready = backend_state.enabled &&
                          !s_sd_reconcile_pending && first_error == ESP_OK;
    const bool journal_attempt = sd_ready && s_journal_dirty;
    const bool primary_attempt = sd_ready && flush_primary &&
                                 s_sd_primary_dirty;
    const bool nvs_attempt = s_nvs_fallback_dirty;
    d1l_store_lock_give(&s_store_lock);

    esp_err_t journal_ret = ESP_OK;
    esp_err_t primary_ret = ESP_OK;
    esp_err_t nvs_ret = ESP_OK;
    if (journal_attempt) {
        operation_attempted = true;
        journal_ret = flush_journal_for_generation(
            backend_state.generation, snapshot_next_seq);
        if (journal_ret == ESP_ERR_NOT_FINISHED) {
            d1l_store_lock_give(&s_persist_io_lock);
            return journal_ret;
        }
        if (journal_ret != ESP_OK && first_error == ESP_OK) {
            first_error = journal_ret;
        }
    }
    if (primary_attempt) {
        operation_attempted = true;
        if (!packet_backend_generation_matches(backend_state.generation)) {
            primary_ret = ESP_ERR_INVALID_STATE;
        } else {
            primary_ret = packet_write_sd_primary_guarded(
                &s_primary_blob_scratch, sizeof(s_primary_blob_scratch),
                backend_state.generation);
            if (primary_ret == ESP_ERR_NOT_FINISHED) {
                d1l_store_lock_give(&s_persist_io_lock);
                return primary_ret;
            }
            if (primary_ret == ESP_OK &&
                !packet_backend_generation_matches(backend_state.generation)) {
                primary_ret = ESP_ERR_INVALID_STATE;
            }
        }
        if (primary_ret != ESP_OK && first_error == ESP_OK) {
            first_error = primary_ret;
        }
    }
    if (nvs_attempt) {
        operation_attempted = true;
        nvs_ret = packet_write_nvs_fallback(
            &s_fallback_blob_scratch, sizeof(s_fallback_blob_scratch));
        if (nvs_ret != ESP_OK && first_error == ESP_OK) {
            first_error = nvs_ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    const bool same_revision = s_revision == snapshot_revision;
    if (journal_attempt) {
        s_journal_last_error = journal_ret;
        if (journal_ret != ESP_OK) {
            s_journal_fail_count++;
            s_sd_history_failed_writes++;
            s_journal_dirty = true;
            if (journal_ret == ESP_ERR_INVALID_STATE) {
                s_sd_reconcile_pending = true;
            }
        }
    }
    if (primary_attempt) {
        s_sd_primary_last_error = primary_ret;
        if (primary_ret == ESP_OK && same_revision) {
            s_sd_primary_dirty = false;
            s_sd_primary_commit_count++;
            s_sd_dirty_count = 0U;
            s_last_sd_flush_ms =
                (uint32_t)(esp_timer_get_time() / 1000ULL);
            s_mutated_since_init = false;
            s_mutations_since_init = 0U;
            s_boot_next_seq = s_next_seq;
        } else if (primary_ret != ESP_OK) {
            s_sd_primary_fail_count++;
            s_sd_primary_dirty = true;
            if (primary_ret == ESP_ERR_INVALID_STATE) {
                s_sd_reconcile_pending = true;
            }
        }
    }
    if (nvs_attempt) {
        s_nvs_fallback_last_error = nvs_ret;
        if (nvs_ret == ESP_OK && same_revision) {
            s_nvs_fallback_dirty = false;
            s_nvs_fallback_commit_count++;
        } else if (nvs_ret != ESP_OK) {
            s_nvs_fallback_fail_count++;
            s_nvs_fallback_dirty = true;
        }
    }
    const bool still_dirty = persistence_dirty_locked(backend_state.enabled);
    if (force && first_error == ESP_OK &&
        (!same_revision || still_dirty)) {
        first_error = ESP_ERR_INVALID_STATE;
    }
    if (operation_attempted && first_error == ESP_OK && !still_dirty) {
        s_persistence_commit_count++;
    } else if (first_error != ESP_OK) {
        s_persistence_fail_count++;
    }
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    return first_error;
}

esp_err_t d1l_packet_log_init(void)
{
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    if (!packet_backend_state(&backend_state)) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_store_lock_take(&s_store_lock);
    clear_ram();
    reset_persistence_state(backend_state.generation, backend_state.enabled);
    s_loaded = false;
    d1l_store_lock_give(&s_store_lock);

    /* The compact NVS mirror is always read independently. SD is reconciled
     * afterwards; corrupt or unreadable SD must never trigger an init erase. */
    size_t len = sizeof(s_fallback_blob_scratch);
    const esp_err_t fallback_ret = packet_read_nvs_fallback(
        &s_fallback_blob_scratch, &len);
    const bool fallback_v2 = fallback_ret == ESP_OK &&
        fallback_blob_v2_is_valid(&s_fallback_blob_scratch, len);
    const bool fallback_current = fallback_ret == ESP_OK &&
        fallback_blob_is_valid(&s_fallback_blob_scratch, len);
    if (fallback_ret != ESP_OK && fallback_ret != ESP_ERR_NOT_FOUND) {
        return fallback_ret;
    }
    if (fallback_ret == ESP_OK && !fallback_v2 && !fallback_current) {
        return ESP_ERR_INVALID_STATE;
    }

    d1l_store_lock_take(&s_store_lock);
    if (fallback_v2) {
        s_fallback_blob_scratch.schema = D1L_PACKET_LOG_SCHEMA;
        load_fallback_blob_into_ram(&s_fallback_blob_scratch);
        s_nvs_fallback_dirty = true;
    } else if (fallback_current) {
        load_fallback_blob_into_ram(&s_fallback_blob_scratch);
    }
    s_revision = 1U;
    s_boot_next_seq = s_next_seq;
    s_loaded = true;
    d1l_store_lock_give(&s_store_lock);

    const esp_err_t ret =
        (backend_state.enabled || fallback_v2) ?
        persist_store(true, true) : ESP_OK;
    return ret;
}

static esp_err_t ensure_packet_log_initialized(void)
{
    d1l_store_lock_take(&s_init_recovery_lock);
    d1l_store_lock_take(&s_store_lock);
    const bool loaded = s_loaded;
    d1l_store_lock_give(&s_store_lock);
    if (loaded) {
        d1l_store_lock_give(&s_init_recovery_lock);
        return ESP_OK;
    }
    const esp_err_t ret = d1l_packet_log_init();
    d1l_store_lock_take(&s_store_lock);
    const bool fallback_loaded = s_loaded;
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_init_recovery_lock);
    return fallback_loaded ? ESP_OK : ret;
}

esp_err_t d1l_packet_log_clear(void)
{
    const esp_err_t init_ret = ensure_packet_log_initialized();
    if (init_ret != ESP_OK) {
        return init_ret;
    }
    d1l_store_lock_take(&s_append_clear_lock);
    /* Close append admission before waiting for persistence I/O. An append
     * that already mutated is ordered before this clear; no later append can
     * be accepted and then erased underneath its caller. */
    d1l_store_lock_take(&s_store_lock);
    if (s_clear_in_progress) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_append_clear_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_clear_in_progress = true;
    d1l_store_lock_give(&s_store_lock);

    d1l_store_lock_take(&s_persist_io_lock);
    /* Media may have changed while clear waited for append/persistence
     * ownership. Sample only after both gates are held so an inserted card
     * cannot be skipped and later re-import the history we reported cleared. */
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    if (!packet_backend_state(&backend_state)) {
        d1l_store_lock_take(&s_store_lock);
        s_clear_in_progress = false;
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        d1l_store_lock_give(&s_append_clear_lock);
        return ESP_ERR_INVALID_STATE;
    }
    d1l_store_lock_take(&s_store_lock);
    if (backend_state.generation != s_last_sd_backend_generation) {
        s_last_sd_backend_generation = backend_state.generation;
        s_sd_reconcile_pending = backend_state.enabled;
    }
    const bool reconcile_required = backend_state.enabled &&
                                    s_sd_reconcile_pending;
    d1l_store_lock_give(&s_store_lock);

    if (reconcile_required) {
        const esp_err_t reconcile_ret = reconcile_sd_primary(
            backend_state.generation);
        if (reconcile_ret != ESP_OK) {
            d1l_store_lock_take(&s_store_lock);
            s_clear_in_progress = false;
            d1l_store_lock_give(&s_store_lock);
            d1l_store_lock_give(&s_persist_io_lock);
            d1l_store_lock_give(&s_append_clear_lock);
            return reconcile_ret;
        }
    }
    d1l_store_lock_take(&s_store_lock);
    const uint64_t clear_revision = s_revision;
    d1l_store_lock_give(&s_store_lock);

    esp_err_t history_ret = ESP_OK;
    esp_err_t primary_ret = ESP_OK;
    if (backend_state.enabled) {
        if (!packet_backend_generation_matches(backend_state.generation)) {
            history_ret = ESP_ERR_INVALID_STATE;
        } else {
            history_ret = clear_sd_history_for_generation(
                backend_state.generation);
        }
        if (history_ret == ESP_OK &&
            packet_backend_generation_matches(backend_state.generation)) {
            primary_ret = d1l_retained_blob_store_erase_sd_primary_guarded(
                D1L_RETAINED_BLOB_STORE_PACKET_LOG, D1L_PACKET_LOG_KEY,
                backend_state.generation);
            if (primary_ret == ESP_OK &&
                !packet_backend_generation_matches(backend_state.generation)) {
                primary_ret = ESP_ERR_INVALID_STATE;
            }
        } else if (history_ret == ESP_OK) {
            history_ret = ESP_ERR_INVALID_STATE;
        }
    }
    esp_err_t nvs_ret = ESP_OK;
    if (history_ret == ESP_OK && primary_ret == ESP_OK) {
        nvs_ret = d1l_retained_blob_store_erase_nvs_fallback(
            D1L_RETAINED_BLOB_STORE_PACKET_LOG, D1L_PACKET_LOG_KEY);
    }
    esp_err_t ret = history_ret != ESP_OK ? history_ret :
                    primary_ret != ESP_OK ? primary_ret : nvs_ret;

    d1l_retained_blob_store_backend_state_t final_backend_state = {0};
    const bool final_backend_known = packet_backend_state(&final_backend_state);
    const bool backend_changed = !final_backend_known ||
        final_backend_state.enabled != backend_state.enabled ||
        final_backend_state.generation != backend_state.generation;
    if (ret == ESP_OK && backend_changed) {
        ret = ESP_ERR_INVALID_STATE;
    }

    d1l_store_lock_take(&s_store_lock);
    if (ret == ESP_OK && s_revision != clear_revision) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        clear_ram();
        s_loaded = true;
        s_revision++;
        s_sd_primary_dirty = false;
        s_sd_reconcile_pending = false;
        s_nvs_fallback_dirty = false;
        s_journal_dirty = false;
        s_journal_next_seq = 1U;
        s_last_sd_backend_generation = backend_state.generation;
        s_boot_next_seq = 1U;
        s_mutations_since_init = 0U;
        s_mutated_since_init = false;
    } else {
        s_persistence_fail_count++;
        s_sd_primary_dirty = backend_changed ?
            (final_backend_known && final_backend_state.enabled) :
            backend_state.enabled;
        s_nvs_fallback_dirty = true;
        s_journal_dirty = history_ret != ESP_OK;
        if (backend_changed || history_ret != ESP_OK || primary_ret != ESP_OK) {
            const esp_err_t sd_error = backend_changed ?
                                       ESP_ERR_INVALID_STATE :
                                       (history_ret != ESP_OK ?
                                        history_ret : primary_ret);
            s_sd_reconcile_pending = true;
            s_sd_primary_fail_count++;
            s_sd_primary_last_error = sd_error;
        }
        if (nvs_ret != ESP_OK) {
            s_nvs_fallback_fail_count++;
            s_nvs_fallback_last_error = nvs_ret;
        }
    }
    s_clear_in_progress = false;
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    d1l_store_lock_give(&s_append_clear_lock);
    return ret;
}

bool d1l_packet_log_append(const d1l_packet_log_entry_t *entry)
{
    return d1l_packet_log_append_raw(entry, NULL, 0);
}

static esp_err_t append_raw_internal(const d1l_packet_log_entry_t *entry,
                                     const uint8_t *raw, size_t raw_len,
                                     bool persist, bool defer_flush,
                                     uint32_t *out_stored_seq)
{
    if (out_stored_seq) {
        *out_stored_seq = 0U;
    }
    if (entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!defer_flush) {
        const esp_err_t init_ret = ensure_packet_log_initialized();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }

    if (defer_flush) {
        if (!d1l_store_lock_try_take(&s_append_clear_lock)) {
            return ESP_ERR_TIMEOUT;
        }
        if (!d1l_store_lock_try_take(&s_store_lock)) {
            d1l_store_lock_give(&s_append_clear_lock);
            return ESP_ERR_TIMEOUT;
        }
    } else {
        d1l_store_lock_take(&s_append_clear_lock);
        d1l_store_lock_take(&s_store_lock);
    }
    if (!s_loaded || s_clear_in_progress) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_append_clear_lock);
        return ESP_ERR_INVALID_STATE;
    }
    d1l_packet_log_entry_t copy = *entry;
    copy.seq = s_next_seq;
    if (copy.uptime_ms == 0) {
        copy.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    }
    sanitize_ascii(copy.direction, sizeof(copy.direction), entry->direction);
    sanitize_ascii(copy.kind, sizeof(copy.kind), entry->kind);
    sanitize_ascii(copy.note, sizeof(copy.note), entry->note);
    sanitize_ascii(copy.raw_hex, sizeof(copy.raw_hex), entry->raw_hex);
    if (copy.direction[0] == '\0') {
        strncpy(copy.direction, "rx", sizeof(copy.direction) - 1U);
    }
    if (copy.kind[0] == '\0') {
        strncpy(copy.kind, "packet", sizeof(copy.kind) - 1U);
    }
    if (raw && raw_len > 0) {
        copy.raw_len = raw_len > UINT16_MAX ? UINT16_MAX : (uint16_t)raw_len;
        copy.raw_truncated = raw_len > D1L_PACKET_LOG_RAW_PREVIEW_BYTES;
        raw_to_hex_preview(copy.raw_hex, sizeof(copy.raw_hex), raw, raw_len);
    }

    if (!persist) {
        /* The UI-only packet is a preview, not part of packet retention. */
        s_volatile_entry = copy;
        s_volatile_valid = true;
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_append_clear_lock);
        return ESP_OK;
    }

    /* The next durable packet supersedes the preview and owns its seq. */
    if (s_next_seq == UINT32_MAX) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_append_clear_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_volatile_valid = false;
    s_next_seq++;

    s_entries[s_head] = copy;
    s_head = (s_head + 1U) % D1L_PACKET_LOG_CAPACITY;
    if (s_count < D1L_PACKET_LOG_CAPACITY) {
        s_count++;
    } else {
        s_dropped_oldest++;
    }
    s_total_written++;
    s_revision++;
    s_mutated_since_init = true;
    s_mutations_since_init++;
    s_nvs_fallback_dirty = true;

    d1l_retained_blob_store_backend_state_t backend_state = {0};
    const bool backend_known = packet_backend_state(&backend_state);
    const bool use_sd = backend_known && backend_state.enabled;
    if (use_sd) {
        s_sd_primary_dirty = true;
        s_sd_dirty_count++;
        s_journal_dirty = s_journal_next_seq < s_next_seq;
    }
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    const bool flush_primary =
        use_sd &&
        (s_sd_dirty_count >= D1L_PACKET_LOG_SD_FLUSH_DIRTY_THRESHOLD ||
         s_last_sd_flush_ms == 0 ||
         now_ms - s_last_sd_flush_ms >= D1L_PACKET_LOG_SD_FLUSH_INTERVAL_MS);
    d1l_store_lock_give(&s_store_lock);
    if (defer_flush) {
        d1l_store_lock_give(&s_append_clear_lock);
        return ESP_OK;
    }
    const esp_err_t ret = persist_store(flush_primary, false);
    if (ret == ESP_OK && out_stored_seq) {
        /* The append/clear owner excludes another packet append while a
         * reconciliation may replay this new row after a newer SD base. The
         * appended row is therefore still the newest retained row even when
         * its originally assigned sequence was deterministically changed. */
        d1l_store_lock_take(&s_store_lock);
        *out_stored_seq = s_next_seq > 1U ? s_next_seq - 1U : 0U;
        d1l_store_lock_give(&s_store_lock);
    }
    d1l_store_lock_give(&s_append_clear_lock);
    return ret;
}

esp_err_t d1l_packet_log_append_raw_checked(const d1l_packet_log_entry_t *entry,
                                            const uint8_t *raw, size_t raw_len,
                                            uint32_t *out_stored_seq)
{
    return append_raw_internal(entry, raw, raw_len, true, false,
                               out_stored_seq);
}

bool d1l_packet_log_append_raw(const d1l_packet_log_entry_t *entry, const uint8_t *raw,
                               size_t raw_len)
{
    return d1l_packet_log_append_raw_checked(entry, raw, raw_len, NULL) == ESP_OK;
}

bool d1l_packet_log_append_raw_deferred(const d1l_packet_log_entry_t *entry,
                                        const uint8_t *raw, size_t raw_len)
{
    return append_raw_internal(entry, raw, raw_len, true, true, NULL) == ESP_OK;
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
bool d1l_packet_log_append_raw_volatile(const d1l_packet_log_entry_t *entry,
                                        const uint8_t *raw, size_t raw_len)
{
    return append_raw_internal(entry, raw, raw_len, false, false, NULL) == ESP_OK;
}
#endif

esp_err_t d1l_packet_log_flush(void)
{
    const esp_err_t ret = ensure_packet_log_initialized();
    if (ret != ESP_OK) {
        return ret;
    }
    return persist_store(true, true);
}

esp_err_t d1l_packet_log_flush_if_due(void)
{
    const esp_err_t ret = ensure_packet_log_initialized();
    if (ret != ESP_OK) {
        return ret;
    }
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    d1l_store_lock_take(&s_store_lock);
    const bool flush_primary =
        s_sd_dirty_count >= D1L_PACKET_LOG_SD_FLUSH_DIRTY_THRESHOLD ||
        s_last_sd_flush_ms == 0U ||
        now_ms - s_last_sd_flush_ms >= D1L_PACKET_LOG_SD_FLUSH_INTERVAL_MS;
    d1l_store_lock_give(&s_store_lock);
    return persist_store(flush_primary, false);
}

d1l_packet_log_stats_t d1l_packet_log_stats(void)
{
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    (void)packet_backend_state(&backend_state);
    d1l_store_lock_take(&s_store_lock);
    const bool sd_history_enabled = backend_state.enabled;
    d1l_packet_log_stats_t stats = {
        .next_seq = s_next_seq,
        .total_written = s_total_written,
        .dropped_oldest = s_dropped_oldest,
        .sd_history_records = sd_history_enabled ? s_sd_history_records : 0,
        .sd_history_failed_writes = s_sd_history_failed_writes,
        .sd_backend_generation = backend_state.generation,
        .persistence_commit_count = s_persistence_commit_count,
        .persistence_fail_count = s_persistence_fail_count,
        .sd_reconcile_count = s_sd_reconcile_count,
        .sd_reconcile_fail_count = s_sd_reconcile_fail_count,
        .sd_reconcile_last_error = s_sd_reconcile_last_error,
        .sd_primary_commit_count = s_sd_primary_commit_count,
        .sd_primary_fail_count = s_sd_primary_fail_count,
        .sd_primary_last_error = s_sd_primary_last_error,
        .nvs_fallback_commit_count = s_nvs_fallback_commit_count,
        .nvs_fallback_fail_count = s_nvs_fallback_fail_count,
        .nvs_fallback_last_error = s_nvs_fallback_last_error,
        .journal_commit_count = s_journal_commit_count,
        .journal_fail_count = s_journal_fail_count,
        .journal_last_error = s_journal_last_error,
        .persistence_revision = s_revision,
        .count = s_count,
        .capacity = D1L_PACKET_LOG_CAPACITY,
        .sd_capacity = D1L_PACKET_LOG_SD_CAPACITY,
        .persistence_dirty = persistence_dirty_locked(sd_history_enabled),
        .sd_primary_dirty = s_sd_primary_dirty,
        .sd_primary_reconcile_pending = s_sd_reconcile_pending,
        .nvs_fallback_dirty = s_nvs_fallback_dirty,
        .journal_dirty = s_journal_dirty,
        .clear_in_progress = s_clear_in_progress,
        .sd_history_enabled = sd_history_enabled,
        .loaded = s_loaded,
    };
    d1l_store_lock_give(&s_store_lock);
    return stats;
}

size_t d1l_packet_log_copy_recent(d1l_packet_log_entry_t *out_entries, size_t max_entries)
{
    if (out_entries == NULL || max_entries == 0) {
        return 0;
    }

    d1l_store_lock_take(&s_store_lock);
    if (s_count == 0 && !s_volatile_valid) {
        d1l_store_lock_give(&s_store_lock);
        return 0;
    }
    const size_t visible_count = s_count + (s_volatile_valid ? 1U : 0U);
    const size_t n = visible_count < max_entries ? visible_count : max_entries;
    const size_t first = visible_count - n;
    const size_t oldest = s_count == 0 ? 0 :
        (s_head + D1L_PACKET_LOG_CAPACITY - s_count) % D1L_PACKET_LOG_CAPACITY;
    for (size_t i = 0; i < n; ++i) {
        const size_t visible_index = first + i;
        out_entries[i] = visible_index < s_count ?
            s_entries[(oldest + visible_index) % D1L_PACKET_LOG_CAPACITY] :
            s_volatile_entry;
    }
    d1l_store_lock_give(&s_store_lock);
    return n;
}

static void reverse_entries(d1l_packet_log_entry_t *entries, size_t count)
{
    if (!entries) {
        return;
    }
    for (size_t i = 0; i < count / 2U; ++i) {
        d1l_packet_log_entry_t tmp = entries[i];
        entries[i] = entries[count - 1U - i];
        entries[count - 1U - i] = tmp;
    }
}

/* The RAM window is authoritative for its sequence range.  SD segment reads
 * may transiently fail while the current RAM copy is still complete, so long
 * history queries must consult RAM before SD for those newest records. */
static bool copy_durable_ram_entry_by_seq(uint32_t seq,
                                          d1l_packet_log_entry_t *out_entry)
{
    if (seq == 0U || !out_entry) {
        return false;
    }

    bool found = false;
    d1l_store_lock_take(&s_store_lock);
    const size_t oldest = s_count == 0U ? 0U :
        (s_head + D1L_PACKET_LOG_CAPACITY - s_count) %
        D1L_PACKET_LOG_CAPACITY;
    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_packet_log_entry_t *entry =
            &s_entries[(oldest + i) % D1L_PACKET_LOG_CAPACITY];
        if (entry->seq == seq) {
            *out_entry = *entry;
            found = true;
            break;
        }
    }
    d1l_store_lock_give(&s_store_lock);
    return found;
}

static bool packet_query_unfiltered(const char *direction, const char *kind,
                                    const char *search_text)
{
    return token_is_any(direction) && token_is_any(kind) && token_is_any(search_text);
}

static size_t query_ram_locked(d1l_packet_log_entry_t *out_entries, size_t max_entries,
                               size_t skip_newest, const char *direction,
                               const char *kind, const char *search_text,
                               size_t *out_total_matches)
{
    if (out_total_matches) {
        *out_total_matches = 0;
    }
    if (s_count == 0 && !s_volatile_valid) {
        return 0;
    }
    size_t total_matches = 0;
    const size_t oldest = s_count == 0 ? 0 :
        (s_head + D1L_PACKET_LOG_CAPACITY - s_count) % D1L_PACKET_LOG_CAPACITY;
    for (size_t i = 0; i < s_count; ++i) {
        const d1l_packet_log_entry_t *entry =
            &s_entries[(oldest + i) % D1L_PACKET_LOG_CAPACITY];
        if (packet_matches(entry, direction, kind, search_text)) {
            total_matches++;
        }
    }
    if (s_volatile_valid &&
        packet_matches(&s_volatile_entry, direction, kind, search_text)) {
        total_matches++;
    }

    if (out_total_matches) {
        *out_total_matches = total_matches;
    }
    if (skip_newest >= total_matches) {
        return 0;
    }

    const size_t available = total_matches - skip_newest;
    const size_t copied = available < max_entries ? available : max_entries;
    const size_t first_match = total_matches - skip_newest - copied;
    const size_t last_match = first_match + copied;
    size_t match_index = 0;
    size_t out_index = 0;
    for (size_t i = 0; i < s_count && out_index < copied; ++i) {
        const d1l_packet_log_entry_t *entry =
            &s_entries[(oldest + i) % D1L_PACKET_LOG_CAPACITY];
        if (!packet_matches(entry, direction, kind, search_text)) {
            continue;
        }
        if (match_index >= first_match && match_index < last_match) {
            out_entries[out_index++] = *entry;
        }
        match_index++;
    }
    if (s_volatile_valid && out_index < copied &&
        packet_matches(&s_volatile_entry, direction, kind, search_text)) {
        if (match_index >= first_match && match_index < last_match) {
            out_entries[out_index++] = s_volatile_entry;
        }
    }
    return out_index;
}

static size_t query_sd_history(d1l_packet_log_entry_t *out_entries, size_t max_entries,
                               size_t skip_newest, const char *direction,
                               const char *kind, const char *search_text,
                               uint32_t newest_seq, uint32_t history_records,
                               uint32_t ram_oldest_seq, uint32_t ram_newest_seq,
                               size_t *out_total_matches, size_t *out_valid_records)
{
    if (out_total_matches) {
        *out_total_matches = 0;
    }
    if (out_valid_records) {
        *out_valid_records = 0;
    }
    if (history_records == 0 || newest_seq == 0) {
        return 0;
    }

    size_t copied = 0;
    size_t total_matches = 0;
    size_t valid_records = 0;
    size_t consecutive_misses = 0;
    const bool unfiltered = packet_query_unfiltered(direction, kind, search_text);
    if (unfiltered && out_total_matches) {
        *out_total_matches = history_records;
    }

    for (uint32_t scanned = 0; scanned < history_records; ++scanned) {
        if (newest_seq <= scanned) {
            break;
        }
        const uint32_t seq = newest_seq - scanned;
        d1l_packet_log_entry_t entry = {0};
        const bool in_ram_window = ram_oldest_seq > 0U &&
            seq >= ram_oldest_seq && seq <= ram_newest_seq;
        const bool read_ok = (in_ram_window &&
                              copy_durable_ram_entry_by_seq(seq, &entry)) ||
            read_sd_history_entry(seq, &entry);
        if (!read_ok) {
            consecutive_misses++;
            if (consecutive_misses >= D1L_PACKET_LOG_HISTORY_MAX_CONSECUTIVE_MISSES) {
                break;
            }
            continue;
        }
        consecutive_misses = 0;
        valid_records++;
        if (!packet_matches(&entry, direction, kind, search_text)) {
            continue;
        }
        if (!unfiltered) {
            total_matches++;
        }
        const size_t newest_match_index = unfiltered ? (size_t)scanned : (total_matches - 1U);
        if (newest_match_index < skip_newest) {
            continue;
        }
        if (copied < max_entries) {
            out_entries[copied++] = entry;
        } else if (!out_total_matches) {
            break;
        }
        if (unfiltered && copied >= max_entries) {
            break;
        }
    }

    if (!unfiltered && out_total_matches) {
        *out_total_matches = total_matches;
    }
    if (out_valid_records) {
        *out_valid_records = valid_records;
    }
    reverse_entries(out_entries, copied);
    return copied;
}

size_t d1l_packet_log_query_page(d1l_packet_log_entry_t *out_entries, size_t max_entries,
                                 size_t skip_newest, const char *direction,
                                 const char *kind, const char *search_text,
                                 size_t *out_total_matches, bool *out_sd_used)
{
    if (out_total_matches) {
        *out_total_matches = 0;
    }
    if (out_sd_used) {
        *out_sd_used = false;
    }
    if (out_entries == NULL || max_entries == 0) {
        return 0;
    }
    if (ensure_packet_log_initialized() != ESP_OK) {
        return 0;
    }

    d1l_store_lock_take(&s_store_lock);
    const bool sd_history_enabled =
        d1l_retained_blob_store_uses_sd(D1L_RETAINED_BLOB_STORE_PACKET_LOG);
    const uint32_t newest_seq = s_next_seq > 0 ? s_next_seq - 1U : 0;
    const uint32_t history_records = sd_history_enabled ? s_sd_history_records : 0;
    const bool use_sd = sd_history_enabled && history_records > s_count;
    const size_t ram_oldest_index = s_count == 0U ? 0U :
        (s_head + D1L_PACKET_LOG_CAPACITY - s_count) %
        D1L_PACKET_LOG_CAPACITY;
    const size_t ram_newest_index = s_count == 0U ? 0U :
        (s_head + D1L_PACKET_LOG_CAPACITY - 1U) %
        D1L_PACKET_LOG_CAPACITY;
    const uint32_t ram_oldest_seq = s_count == 0U ? 0U :
        s_entries[ram_oldest_index].seq;
    const uint32_t ram_newest_seq = s_count == 0U ? 0U :
        s_entries[ram_newest_index].seq;
    const bool volatile_matches = s_volatile_valid &&
        packet_matches(&s_volatile_entry, direction, kind, search_text);
    const d1l_packet_log_entry_t volatile_entry = s_volatile_entry;
    if (!use_sd) {
        const size_t copied = query_ram_locked(out_entries, max_entries, skip_newest,
                                              direction, kind, search_text,
                                              out_total_matches);
        d1l_store_lock_give(&s_store_lock);
        return copied;
    }
    d1l_store_lock_give(&s_store_lock);

    const bool include_volatile = volatile_matches && skip_newest == 0;
    const size_t history_skip = skip_newest -
        ((volatile_matches && skip_newest > 0) ? 1U : 0U);
    const size_t history_limit = max_entries - (include_volatile ? 1U : 0U);
    size_t valid_records = 0;
    size_t history_matches = 0;
    size_t *history_matches_out = out_total_matches ? &history_matches : NULL;
    size_t copied = query_sd_history(out_entries, history_limit, history_skip,
                                     direction, kind, search_text,
                                     newest_seq, history_records,
                                     ram_oldest_seq, ram_newest_seq,
                                     history_matches_out, &valid_records);
    if (include_volatile) {
        out_entries[copied++] = volatile_entry;
    }
    if (out_total_matches) {
        *out_total_matches = history_matches + (volatile_matches ? 1U : 0U);
    }
    if (out_sd_used) {
        *out_sd_used = valid_records > 0;
    }
    if (valid_records > 0U || skip_newest > 0U) {
        return copied;
    }

    d1l_store_lock_take(&s_store_lock);
    const size_t fallback = query_ram_locked(out_entries, max_entries, skip_newest,
                                            direction, kind, search_text,
                                            out_total_matches);
    d1l_store_lock_give(&s_store_lock);
    return fallback;
}

size_t d1l_packet_log_query(d1l_packet_log_entry_t *out_entries, size_t max_entries,
                            const char *direction, const char *kind, const char *search_text)
{
    return d1l_packet_log_query_page(out_entries, max_entries, 0, direction, kind,
                                     search_text, NULL, NULL);
}

esp_err_t d1l_packet_log_find_by_seq(uint32_t seq, d1l_packet_log_entry_t *out_entry)
{
    if (seq == 0 || out_entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t init_ret = ensure_packet_log_initialized();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    d1l_store_lock_take(&s_store_lock);
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
    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    const uint32_t newest_seq = stats.next_seq > 0 ? stats.next_seq - 1U : 0;
    const uint32_t oldest_seq =
        stats.sd_history_records > newest_seq ? 1U : newest_seq - stats.sd_history_records + 1U;
    if (stats.sd_history_enabled && seq >= oldest_seq && seq <= newest_seq &&
        read_sd_history_entry(seq, out_entry)) {
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
