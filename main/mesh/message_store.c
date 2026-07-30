#include "message_store.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "mesh/channel_store.h"
#include "mesh/store_lock.h"
#include "storage/retained_blob_store.h"

#define D1L_MESSAGE_STORE_ID D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES
#define D1L_MESSAGE_STORE_KEY "public"
#define D1L_MESSAGE_STORE_SCHEMA 5U
#define D1L_MESSAGE_STORE_SCHEMA_V4 4U
#define D1L_MESSAGE_STORE_SCHEMA_V3 3U
#define D1L_MESSAGE_STORE_SCHEMA_V2 2U
#define D1L_MESSAGE_STORE_SCHEMA_V1 1U
#define D1L_MESSAGE_TEXT_LEN_V1 96U

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    char direction[4];
    char author[D1L_MESSAGE_AUTHOR_LEN];
    char text[D1L_MESSAGE_TEXT_LEN];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool delivered;
} d1l_message_entry_v4_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    uint32_t epoch;
    uint32_t content_revision;
    uint64_t clear_lineage;
    d1l_message_entry_v4_t entries[D1L_MESSAGE_STORE_CAPACITY];
} d1l_message_store_blob_v4_t;

_Static_assert(sizeof(d1l_message_entry_v4_t) == 188U,
               "message schema v4 entry layout changed");
_Static_assert(offsetof(d1l_message_entry_v4_t, text) == 36U,
               "message schema v4 text offset changed");
_Static_assert(offsetof(d1l_message_entry_v4_t, rssi_dbm) == 176U,
               "message schema v4 signal offset changed");
_Static_assert(offsetof(d1l_message_entry_v4_t, delivered) == 186U,
               "message schema v4 tail offset changed");
_Static_assert(offsetof(d1l_message_store_blob_v4_t, entries) == 40U,
               "message schema v4 entries offset changed");
_Static_assert(sizeof(d1l_message_store_blob_v4_t) == 3048U,
               "message schema v4 blob layout changed");

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    uint32_t epoch;
    uint32_t content_revision;
    uint64_t clear_lineage;
    d1l_message_entry_t entries[D1L_MESSAGE_STORE_CAPACITY];
} d1l_message_store_blob_t;

_Static_assert(offsetof(d1l_message_entry_t, reserved) == 187U,
               "message schema v5 reserved offset changed");
_Static_assert(offsetof(d1l_message_entry_t, channel_id) == 192U,
               "message schema v5 channel identity offset changed");
_Static_assert(sizeof(d1l_message_entry_t) == 200U,
               "message schema v5 entry layout changed");
_Static_assert(offsetof(d1l_message_store_blob_t, entries) == 40U,
               "message schema v5 entries offset changed");
_Static_assert(sizeof(d1l_message_store_blob_t) == 3240U,
               "message schema v5 blob layout changed");

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    uint32_t epoch;
    uint32_t content_revision;
    d1l_message_entry_v4_t entries[D1L_MESSAGE_STORE_CAPACITY];
} d1l_message_store_blob_v3_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    d1l_message_entry_v4_t entries[D1L_MESSAGE_STORE_CAPACITY];
} d1l_message_store_blob_v2_t;

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    char direction[4];
    char author[D1L_MESSAGE_AUTHOR_LEN];
    char text[D1L_MESSAGE_TEXT_LEN_V1];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool delivered;
} d1l_message_entry_v1_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    d1l_message_entry_v1_t entries[D1L_MESSAGE_STORE_CAPACITY];
} d1l_message_store_blob_v1_t;

typedef union {
    uint32_t schema;
    d1l_message_store_blob_t current;
    d1l_message_store_blob_v4_t v4;
    d1l_message_store_blob_v3_t v3;
    d1l_message_store_blob_v2_t v2;
    d1l_message_store_blob_v1_t legacy;
} d1l_message_store_read_buffer_t;

typedef struct {
    uint64_t revision;
    bool sd_attempt;
    bool nvs_attempt;
    d1l_message_store_blob_t blob;
} d1l_message_persist_snapshot_t;

static d1l_message_entry_t s_entries[D1L_MESSAGE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static size_t s_head;
static size_t s_count;
static uint32_t s_next_seq = 1;
static uint32_t s_total_written;
static uint32_t s_dropped_oldest;
static uint32_t s_epoch = 1U;
static uint32_t s_content_revision = 1U;
static uint64_t s_clear_lineage;
static d1l_message_entry_t s_volatile_entry;
static bool s_volatile_valid;
static bool s_loaded;
static d1l_message_store_blob_t s_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_message_store_blob_t s_sd_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_message_store_blob_t s_nvs_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_message_store_read_buffer_t s_read_scratch EXT_RAM_BSS_ATTR;
static d1l_message_persist_snapshot_t s_persist_snapshot EXT_RAM_BSS_ATTR;
static d1l_message_entry_t
    s_merge_scratch[D1L_MESSAGE_STORE_CAPACITY * 2U] EXT_RAM_BSS_ATTR;
static bool s_persistence_dirty;
static bool s_sd_primary_dirty;
static bool s_sd_reconcile_pending;
static bool s_nvs_fallback_dirty;
static bool s_retry_pending;
static bool s_persist_attempted_this_boot;
static bool s_mutated_since_init;
static bool s_device_lineage_authoritative;
static uint32_t s_last_sd_backend_generation;
static uint32_t s_boot_next_seq = 1U;
static uint32_t s_mutations_since_init;
static uint32_t s_last_persist_attempt_ms;
static uint32_t s_persistence_commit_count;
static uint32_t s_persistence_fail_count;
static uint32_t s_persistence_stale_snapshot_count;
static uint32_t s_sd_primary_commit_count;
static uint32_t s_sd_primary_fail_count;
static esp_err_t s_sd_primary_last_error;
static uint32_t s_nvs_fallback_commit_count;
static uint32_t s_nvs_fallback_fail_count;
static esp_err_t s_nvs_fallback_last_error;
static uint64_t s_revision;
static d1l_store_lock_t s_store_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_persist_io_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_init_recovery_lock = D1L_STORE_LOCK_INITIALIZER;

static void load_blob_into_ram(const d1l_message_store_blob_t *blob);
static esp_err_t persist_store(bool force);
static esp_err_t adopt_primary_with_live_overlay_locked(
    const d1l_message_store_blob_t *primary);

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

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static bool contains_casefold(const char *haystack, const char *needle)
{
    if (!needle || needle[0] == '\0') {
        return true;
    }
    if (!haystack) {
        return false;
    }
    for (size_t i = 0; haystack[i]; ++i) {
        size_t j = 0;
        while (needle[j] && haystack[i + j] &&
               ascii_lower(haystack[i + j]) == ascii_lower(needle[j])) {
            j++;
        }
        if (needle[j] == '\0') {
            return true;
        }
    }
    return false;
}

static bool message_matches_query(const d1l_message_entry_t *entry, const char *query)
{
    return !query || query[0] == '\0' ||
           contains_casefold(entry->author, query) ||
           contains_casefold(entry->text, query) ||
           contains_casefold(entry->direction, query);
}

static void clear_ram(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_head = 0;
    s_count = 0;
    s_next_seq = 1;
    s_total_written = 0;
    s_dropped_oldest = 0;
    memset(&s_volatile_entry, 0, sizeof(s_volatile_entry));
    s_volatile_valid = false;
}

static uint64_t new_clear_lineage(void)
{
    uint64_t lineage = 0U;
    while (lineage == 0U) {
        lineage = ((uint64_t)esp_random() << 32U) | esp_random();
    }
    return lineage;
}

static bool persistence_dirty_locked(bool sd_primary_required)
{
    return s_nvs_fallback_dirty ||
           (sd_primary_required &&
            (s_sd_primary_dirty || s_sd_reconcile_pending));
}

static void reset_persistence_state(uint32_t backend_generation)
{
    s_epoch = 1U;
    s_content_revision = 1U;
    s_clear_lineage = 0U;
    s_persistence_dirty = false;
    s_sd_primary_dirty = false;
    s_sd_reconcile_pending = false;
    s_nvs_fallback_dirty = false;
    s_retry_pending = false;
    s_persist_attempted_this_boot = false;
    s_mutated_since_init = false;
    s_device_lineage_authoritative = false;
    s_last_sd_backend_generation = backend_generation;
    s_boot_next_seq = 1U;
    s_mutations_since_init = 0U;
    s_last_persist_attempt_ms = 0U;
    s_persistence_commit_count = 0U;
    s_persistence_fail_count = 0U;
    s_persistence_stale_snapshot_count = 0U;
    s_sd_primary_commit_count = 0U;
    s_sd_primary_fail_count = 0U;
    s_sd_primary_last_error = ESP_OK;
    s_nvs_fallback_commit_count = 0U;
    s_nvs_fallback_fail_count = 0U;
    s_nvs_fallback_last_error = ESP_OK;
    s_revision = 0U;
}

static void fill_blob(d1l_message_store_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->schema = D1L_MESSAGE_STORE_SCHEMA;
    blob->epoch = s_epoch;
    blob->content_revision = s_content_revision;
    blob->clear_lineage = s_clear_lineage;
    blob->next_seq = s_next_seq;
    blob->total_written = s_total_written;
    blob->dropped_oldest = s_dropped_oldest;
    const size_t oldest = s_count == 0 ? 0 :
        (s_head + D1L_MESSAGE_STORE_CAPACITY - s_count) % D1L_MESSAGE_STORE_CAPACITY;
    for (size_t i = 0; i < s_count; ++i) {
        const d1l_message_entry_t *entry =
            &s_entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        if (blob->count < D1L_MESSAGE_STORE_CAPACITY) {
            blob->entries[blob->count++] = *entry;
        }
    }
    blob->head = blob->count % D1L_MESSAGE_STORE_CAPACITY;
}

static size_t blob_oldest_index(uint32_t head, uint32_t count)
{
    return count == 0U ? 0U :
        (head + D1L_MESSAGE_STORE_CAPACITY - count) %
        D1L_MESSAGE_STORE_CAPACITY;
}

static bool persisted_ascii_is_valid(const char *text, size_t capacity,
                                     bool require_nonempty)
{
    if (!text || capacity == 0U) {
        return false;
    }
    size_t length = 0U;
    while (length < capacity && text[length] != '\0') {
        const unsigned char c = (unsigned char)text[length];
        if (c < 32U || c > 126U || c == '"' || c == '\\') {
            return false;
        }
        length++;
    }
    return length < capacity && (!require_nonempty || length > 0U);
}

static bool entry_is_valid(const d1l_message_entry_t *entry)
{
    static const uint8_t zero_reserved[5] = {0};
    return entry && entry->seq > 0U && entry->channel_id != 0U &&
           memcmp(entry->reserved, zero_reserved,
                  sizeof(entry->reserved)) == 0 &&
           persisted_ascii_is_valid(entry->direction,
                                    sizeof(entry->direction), true) &&
           persisted_ascii_is_valid(entry->author,
                                    sizeof(entry->author), true) &&
           d1l_user_text_validate_bounded(entry->text,
                                          sizeof(entry->text), false).result ==
               D1L_USER_TEXT_OK;
}

static bool entry_v1_is_valid(const d1l_message_entry_v1_t *entry)
{
    return entry && entry->seq > 0U &&
           persisted_ascii_is_valid(entry->direction,
                                    sizeof(entry->direction), true) &&
           persisted_ascii_is_valid(entry->author,
                                    sizeof(entry->author), true) &&
           persisted_ascii_is_valid(entry->text,
                                    sizeof(entry->text), true);
}

static bool legacy_entry_is_valid(const d1l_message_entry_v4_t *entry)
{
    return entry && entry->seq > 0U &&
           persisted_ascii_is_valid(entry->direction,
                                    sizeof(entry->direction), true) &&
           persisted_ascii_is_valid(entry->author,
                                    sizeof(entry->author), true) &&
           persisted_ascii_is_valid(entry->text,
                                    sizeof(entry->text), true);
}

static bool entry_v4_is_valid(const d1l_message_entry_v4_t *entry)
{
    return entry && entry->seq > 0U &&
           persisted_ascii_is_valid(entry->direction,
                                    sizeof(entry->direction), true) &&
           persisted_ascii_is_valid(entry->author,
                                    sizeof(entry->author), true) &&
           d1l_user_text_validate_bounded(entry->text,
                                          sizeof(entry->text), false).result ==
               D1L_USER_TEXT_OK;
}

static bool blob_v4_is_valid(const d1l_message_store_blob_v4_t *blob,
                             size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_MESSAGE_STORE_SCHEMA_V4 ||
        blob->epoch == 0U || blob->content_revision == 0U ||
        blob->head >= D1L_MESSAGE_STORE_CAPACITY ||
        blob->count > D1L_MESSAGE_STORE_CAPACITY ||
        blob->next_seq == 0U || blob->total_written < blob->count) {
        return false;
    }
    const size_t oldest = blob_oldest_index(blob->head, blob->count);
    uint32_t previous_seq = 0U;
    for (size_t i = 0U; i < blob->count; ++i) {
        const d1l_message_entry_v4_t *entry =
            &blob->entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        if (!entry_v4_is_valid(entry) || entry->seq <= previous_seq ||
            entry->seq >= blob->next_seq) {
            return false;
        }
        previous_seq = entry->seq;
    }
    return true;
}

static bool blob_is_valid(const d1l_message_store_blob_t *blob, size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_MESSAGE_STORE_SCHEMA ||
        blob->epoch == 0U || blob->content_revision == 0U ||
        blob->head >= D1L_MESSAGE_STORE_CAPACITY ||
        blob->count > D1L_MESSAGE_STORE_CAPACITY ||
        blob->next_seq == 0U || blob->total_written < blob->count) {
        return false;
    }
    const size_t oldest = blob_oldest_index(blob->head, blob->count);
    uint32_t previous_seq = 0U;
    for (size_t i = 0U; i < blob->count; ++i) {
        const d1l_message_entry_t *entry =
            &blob->entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        if (!entry_is_valid(entry) || entry->seq <= previous_seq ||
            entry->seq >= blob->next_seq) {
            return false;
        }
        previous_seq = entry->seq;
    }
    return true;
}

static bool blob_v2_is_valid(const d1l_message_store_blob_v2_t *blob,
                             size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_MESSAGE_STORE_SCHEMA_V2 ||
        blob->head >= D1L_MESSAGE_STORE_CAPACITY ||
        blob->count > D1L_MESSAGE_STORE_CAPACITY ||
        blob->next_seq == 0U || blob->total_written < blob->count) {
        return false;
    }
    const size_t oldest = blob_oldest_index(blob->head, blob->count);
    uint32_t previous_seq = 0U;
    for (size_t i = 0U; i < blob->count; ++i) {
        const d1l_message_entry_v4_t *entry =
            &blob->entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        if (!legacy_entry_is_valid(entry) || entry->seq <= previous_seq ||
            entry->seq >= blob->next_seq) {
            return false;
        }
        previous_seq = entry->seq;
    }
    return true;
}

static bool blob_v3_is_valid(const d1l_message_store_blob_v3_t *blob,
                             size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_MESSAGE_STORE_SCHEMA_V3 ||
        blob->epoch == 0U || blob->content_revision == 0U ||
        blob->head >= D1L_MESSAGE_STORE_CAPACITY ||
        blob->count > D1L_MESSAGE_STORE_CAPACITY ||
        blob->next_seq == 0U || blob->total_written < blob->count) {
        return false;
    }
    const size_t oldest = blob_oldest_index(blob->head, blob->count);
    uint32_t previous_seq = 0U;
    for (size_t i = 0U; i < blob->count; ++i) {
        const d1l_message_entry_v4_t *entry =
            &blob->entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        if (!legacy_entry_is_valid(entry) || entry->seq <= previous_seq ||
            entry->seq >= blob->next_seq) {
            return false;
        }
        previous_seq = entry->seq;
    }
    return true;
}

static bool blob_v1_is_valid(const d1l_message_store_blob_v1_t *blob, size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_MESSAGE_STORE_SCHEMA_V1 ||
        blob->head >= D1L_MESSAGE_STORE_CAPACITY ||
        blob->count > D1L_MESSAGE_STORE_CAPACITY ||
        blob->next_seq == 0U || blob->total_written < blob->count) {
        return false;
    }
    const size_t oldest = blob_oldest_index(blob->head, blob->count);
    uint32_t previous_seq = 0U;
    for (size_t i = 0U; i < blob->count; ++i) {
        const d1l_message_entry_v1_t *entry =
            &blob->entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        if (!entry_v1_is_valid(entry) || entry->seq <= previous_seq ||
            entry->seq >= blob->next_seq) {
            return false;
        }
        previous_seq = entry->seq;
    }
    return true;
}

static uint32_t migration_revision(uint32_t total_written)
{
    return total_written == UINT32_MAX ? UINT32_MAX : total_written + 1U;
}

static void convert_v4_entry(const d1l_message_entry_v4_t *legacy,
                             d1l_message_entry_t *current)
{
    memset(current, 0, sizeof(*current));
    current->seq = legacy->seq;
    current->uptime_ms = legacy->uptime_ms;
    memcpy(current->direction, legacy->direction,
           sizeof(current->direction));
    memcpy(current->author, legacy->author, sizeof(current->author));
    memcpy(current->text, legacy->text, sizeof(current->text));
    current->rssi_dbm = legacy->rssi_dbm;
    current->snr_tenths = legacy->snr_tenths;
    current->path_hash_bytes = legacy->path_hash_bytes;
    current->path_hops = legacy->path_hops;
    current->delivered = legacy->delivered;
    current->channel_id = D1L_CHANNEL_PUBLIC_ID;
}

static void convert_v4_blob(const d1l_message_store_blob_v4_t *legacy,
                            d1l_message_store_blob_t *current)
{
    memset(current, 0, sizeof(*current));
    current->schema = D1L_MESSAGE_STORE_SCHEMA;
    current->epoch = legacy->epoch;
    current->content_revision = legacy->content_revision;
    current->clear_lineage = legacy->clear_lineage;
    current->next_seq = legacy->next_seq;
    current->total_written = legacy->total_written;
    current->dropped_oldest = legacy->dropped_oldest;
    current->count = legacy->count;
    current->head = legacy->count % D1L_MESSAGE_STORE_CAPACITY;
    const size_t oldest = blob_oldest_index(legacy->head, legacy->count);
    for (size_t i = 0U; i < legacy->count; ++i) {
        convert_v4_entry(
            &legacy->entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY],
            &current->entries[i]);
    }
}

static void convert_v3_blob(const d1l_message_store_blob_v3_t *legacy,
                            d1l_message_store_blob_t *current)
{
    memset(current, 0, sizeof(*current));
    current->schema = D1L_MESSAGE_STORE_SCHEMA;
    current->epoch = legacy->epoch;
    current->content_revision = legacy->content_revision;
    current->clear_lineage = 0U;
    current->next_seq = legacy->next_seq;
    current->total_written = legacy->total_written;
    current->dropped_oldest = legacy->dropped_oldest;
    current->count = legacy->count;
    current->head = legacy->count % D1L_MESSAGE_STORE_CAPACITY;
    const size_t oldest = blob_oldest_index(legacy->head, legacy->count);
    for (size_t i = 0U; i < legacy->count; ++i) {
        convert_v4_entry(
            &legacy->entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY],
            &current->entries[i]);
    }
}

static void convert_v2_blob(const d1l_message_store_blob_v2_t *legacy,
                            d1l_message_store_blob_t *current)
{
    memset(current, 0, sizeof(*current));
    current->schema = D1L_MESSAGE_STORE_SCHEMA;
    current->epoch = 1U;
    current->content_revision = migration_revision(legacy->total_written);
    current->clear_lineage = 0U;
    current->next_seq = legacy->next_seq;
    current->total_written = legacy->total_written;
    current->dropped_oldest = legacy->dropped_oldest;
    current->count = legacy->count;
    current->head = legacy->count % D1L_MESSAGE_STORE_CAPACITY;
    const size_t oldest = blob_oldest_index(legacy->head, legacy->count);
    for (size_t i = 0U; i < legacy->count; ++i) {
        convert_v4_entry(
            &legacy->entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY],
            &current->entries[i]);
    }
}

static void convert_v1_blob(const d1l_message_store_blob_v1_t *legacy,
                            d1l_message_store_blob_t *current)
{
    memset(current, 0, sizeof(*current));
    current->schema = D1L_MESSAGE_STORE_SCHEMA;
    current->epoch = 1U;
    current->content_revision = migration_revision(legacy->total_written);
    current->clear_lineage = 0U;
    current->next_seq = legacy->next_seq;
    current->total_written = legacy->total_written;
    current->dropped_oldest = legacy->dropped_oldest;
    current->count = legacy->count;
    current->head = legacy->count % D1L_MESSAGE_STORE_CAPACITY;
    const size_t oldest = blob_oldest_index(legacy->head, legacy->count);
    for (size_t i = 0; i < legacy->count; ++i) {
        const d1l_message_entry_v1_t *source =
            &legacy->entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        current->entries[i].seq = source->seq;
        current->entries[i].uptime_ms = source->uptime_ms;
        sanitize_ascii(current->entries[i].direction,
                       sizeof(current->entries[i].direction),
                       source->direction);
        sanitize_ascii(current->entries[i].author,
                       sizeof(current->entries[i].author), source->author);
        sanitize_ascii(current->entries[i].text,
                       sizeof(current->entries[i].text), source->text);
        current->entries[i].rssi_dbm = source->rssi_dbm;
        current->entries[i].snr_tenths = source->snr_tenths;
        current->entries[i].path_hash_bytes = source->path_hash_bytes;
        current->entries[i].path_hops = source->path_hops;
        current->entries[i].delivered = source->delivered;
        current->entries[i].channel_id = D1L_CHANNEL_PUBLIC_ID;
    }
}

static esp_err_t read_blob_target(bool sd_primary,
                                  d1l_message_store_blob_t *out_blob,
                                  bool *out_found,
                                  bool *out_migrated)
{
    if (!out_blob || !out_found || !out_migrated) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_message_store_read_buffer_t *buffer = &s_read_scratch;
    memset(buffer, 0, sizeof(*buffer));
    size_t len = sizeof(*buffer);
    const esp_err_t ret = sd_primary ?
        d1l_retained_blob_store_read_sd_primary(
            D1L_MESSAGE_STORE_ID, D1L_MESSAGE_STORE_KEY, buffer, &len) :
        d1l_retained_blob_store_read_nvs_fallback(
            D1L_MESSAGE_STORE_ID, D1L_MESSAGE_STORE_KEY, buffer, &len);
    *out_found = false;
    *out_migrated = false;
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (buffer->schema == D1L_MESSAGE_STORE_SCHEMA &&
        blob_is_valid(&buffer->current, len)) {
        *out_blob = buffer->current;
        *out_found = true;
        return ESP_OK;
    }
    if (buffer->schema == D1L_MESSAGE_STORE_SCHEMA_V4 &&
        blob_v4_is_valid(&buffer->v4, len)) {
        convert_v4_blob(&buffer->v4, out_blob);
        *out_found = true;
        *out_migrated = true;
        return ESP_OK;
    }
    if (buffer->schema == D1L_MESSAGE_STORE_SCHEMA_V3 &&
        blob_v3_is_valid(&buffer->v3, len)) {
        convert_v3_blob(&buffer->v3, out_blob);
        *out_found = true;
        *out_migrated = true;
        return ESP_OK;
    }
    if (buffer->schema == D1L_MESSAGE_STORE_SCHEMA_V2 &&
        blob_v2_is_valid(&buffer->v2, len)) {
        convert_v2_blob(&buffer->v2, out_blob);
        *out_found = true;
        *out_migrated = true;
        return ESP_OK;
    }
    if (buffer->schema == D1L_MESSAGE_STORE_SCHEMA_V1 &&
        blob_v1_is_valid(&buffer->legacy, len)) {
        convert_v1_blob(&buffer->legacy, out_blob);
        *out_found = true;
        *out_migrated = true;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

static void load_blob_into_ram(const d1l_message_store_blob_t *blob)
{
    memcpy(s_entries, blob->entries, sizeof(s_entries));
    s_head = blob->head;
    s_count = blob->count;
    s_next_seq = blob->next_seq;
    s_total_written = blob->total_written;
    s_dropped_oldest = blob->dropped_oldest;
    s_epoch = blob->epoch;
    s_content_revision = blob->content_revision;
    s_clear_lineage = blob->clear_lineage;
}

static bool entries_equal(const d1l_message_entry_t *left,
                          const d1l_message_entry_t *right)
{
    return left && right && left->seq == right->seq &&
           left->channel_id == right->channel_id &&
           left->uptime_ms == right->uptime_ms &&
           strcmp(left->direction, right->direction) == 0 &&
           strcmp(left->author, right->author) == 0 &&
           strcmp(left->text, right->text) == 0 &&
           left->rssi_dbm == right->rssi_dbm &&
           left->snr_tenths == right->snr_tenths &&
           left->path_hash_bytes == right->path_hash_bytes &&
           left->path_hops == right->path_hops &&
           left->delivered == right->delivered;
}

static bool blobs_equivalent(const d1l_message_store_blob_t *left,
                             const d1l_message_store_blob_t *right)
{
    if (!left || !right || left->clear_lineage != right->clear_lineage ||
        left->epoch != right->epoch ||
        left->content_revision != right->content_revision ||
        left->next_seq != right->next_seq ||
        left->total_written != right->total_written ||
        left->dropped_oldest != right->dropped_oldest ||
        left->count != right->count) {
        return false;
    }
    const size_t left_oldest = blob_oldest_index(left->head, left->count);
    const size_t right_oldest = blob_oldest_index(right->head, right->count);
    for (size_t i = 0U; i < left->count; ++i) {
        if (!entries_equal(
                &left->entries[(left_oldest + i) % D1L_MESSAGE_STORE_CAPACITY],
                &right->entries[(right_oldest + i) % D1L_MESSAGE_STORE_CAPACITY])) {
            return false;
        }
    }
    return true;
}

static bool blob_is_newer(const d1l_message_store_blob_t *left,
                          const d1l_message_store_blob_t *right)
{
    /* Lineage authority is resolved from the device-local NVS source before
     * this ordering helper is used. Random lineage values are identities, not
     * comparable clocks. */
    if (left->epoch != right->epoch) {
        return left->epoch > right->epoch;
    }
    if (left->content_revision != right->content_revision) {
        return left->content_revision > right->content_revision;
    }
    if (left->total_written != right->total_written) {
        return left->total_written > right->total_written;
    }
    return left->next_seq > right->next_seq;
}

static size_t copy_blob_entries(const d1l_message_store_blob_t *blob,
                                d1l_message_entry_t *out_entries,
                                size_t max_entries)
{
    if (!blob || !out_entries || max_entries == 0U) {
        return 0U;
    }
    const size_t count = blob->count < max_entries ? blob->count : max_entries;
    const size_t oldest = blob_oldest_index(blob->head, blob->count);
    for (size_t i = 0U; i < count; ++i) {
        out_entries[i] =
            blob->entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
    }
    return count;
}

static esp_err_t merge_blob_into_ram_locked(const d1l_message_store_blob_t *blob,
                                            bool *out_changed)
{
    if (!blob || !out_changed) {
        return ESP_ERR_INVALID_ARG;
    }
    fill_blob(&s_blob_scratch);
    if (blob->clear_lineage != s_clear_lineage) {
        /* A nonzero in-RAM lineage came from device-local NVS (or a clear
         * accepted for NVS persistence) and is authoritative over removable
         * media. Different random tokens must never be ordered numerically. */
        *out_changed = false;
        return ESP_OK;
    }
    if (blob->epoch < s_epoch) {
        *out_changed = false;
        return ESP_OK;
    }
    if (blob->epoch > s_epoch) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t merged_count = copy_blob_entries(&s_blob_scratch, s_merge_scratch,
                                            D1L_MESSAGE_STORE_CAPACITY * 2U);
    uint32_t next_seq = s_next_seq > blob->next_seq ?
                        s_next_seq : blob->next_seq;
    uint32_t additional_count = 0U;
    uint32_t retained_live_count = 0U;
    const size_t incoming_oldest = blob_oldest_index(blob->head, blob->count);
    if (s_mutated_since_init) {
        for (size_t current = 0U; current < merged_count; ++current) {
            if (s_merge_scratch[current].seq < s_boot_next_seq) {
                continue;
            }
            retained_live_count++;
            bool exact_on_sd = false;
            for (size_t source = 0U; source < blob->count; ++source) {
                const d1l_message_entry_t *candidate = &blob->entries[
                    (incoming_oldest + source) % D1L_MESSAGE_STORE_CAPACITY];
                if (entries_equal(&s_merge_scratch[current], candidate)) {
                    exact_on_sd = true;
                    break;
                }
            }
            if (!exact_on_sd) {
                additional_count++;
            }
        }
        if (s_mutations_since_init > retained_live_count) {
            additional_count += s_mutations_since_init - retained_live_count;
        }
    }
    for (size_t i = 0U; i < blob->count; ++i) {
        d1l_message_entry_t incoming =
            blob->entries[(incoming_oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        bool duplicate = false;
        for (size_t j = 0U; j < merged_count; ++j) {
            if (s_merge_scratch[j].seq != incoming.seq) {
                continue;
            }
            if (!entries_equal(&s_merge_scratch[j], &incoming)) {
                if (next_seq == UINT32_MAX) {
                    return ESP_ERR_INVALID_SIZE;
                }
                const bool current_is_live = s_mutated_since_init &&
                    s_merge_scratch[j].seq >= s_boot_next_seq;
                if (current_is_live) {
                    if (merged_count >= D1L_MESSAGE_STORE_CAPACITY * 2U) {
                        return ESP_ERR_INVALID_SIZE;
                    }
                    d1l_message_entry_t live = s_merge_scratch[j];
                    live.seq = next_seq++;
                    s_merge_scratch[j] = incoming;
                    s_merge_scratch[merged_count++] = live;
                    duplicate = true;
                } else {
                    incoming.seq = next_seq++;
                    additional_count++;
                }
                break;
            }
            duplicate = true;
            break;
        }
        if (!duplicate) {
            s_merge_scratch[merged_count++] = incoming;
        }
    }
    for (size_t i = 1U; i < merged_count; ++i) {
        const d1l_message_entry_t ordered = s_merge_scratch[i];
        size_t position = i;
        while (position > 0U &&
               s_merge_scratch[position - 1U].seq > ordered.seq) {
            s_merge_scratch[position] = s_merge_scratch[position - 1U];
            position--;
        }
        s_merge_scratch[position] = ordered;
    }

    const size_t kept = merged_count < D1L_MESSAGE_STORE_CAPACITY ?
        merged_count : D1L_MESSAGE_STORE_CAPACITY;
    const size_t first = merged_count - kept;
    memset(s_entries, 0, sizeof(s_entries));
    for (size_t i = 0U; i < kept; ++i) {
        s_entries[i] = s_merge_scratch[first + i];
    }
    s_count = kept;
    s_head = kept % D1L_MESSAGE_STORE_CAPACITY;
    s_next_seq = next_seq;
    const uint32_t local_base_total = s_mutated_since_init ?
        (s_total_written >= s_mutations_since_init ?
         s_total_written - s_mutations_since_init : 0U) :
        s_total_written;
    s_total_written = blob->total_written > local_base_total ?
        blob->total_written : local_base_total;
    if (additional_count > 0U) {
        s_total_written = s_total_written > UINT32_MAX - additional_count ?
            UINT32_MAX : s_total_written + additional_count;
    }
    if (s_total_written < kept) {
        s_total_written = (uint32_t)kept;
    }
    s_dropped_oldest = s_total_written >= s_count ?
        s_total_written - (uint32_t)s_count : 0U;
    if (blob->content_revision > s_content_revision) {
        s_content_revision = blob->content_revision;
    }
    fill_blob(&s_nvs_blob_scratch);
    *out_changed = !blobs_equivalent(&s_blob_scratch, &s_nvs_blob_scratch);
    if (*out_changed && s_content_revision < UINT32_MAX) {
        s_content_revision++;
    }
    return ESP_OK;
}

static esp_err_t append_replayed_entry_locked(const d1l_message_entry_t *source)
{
    if (!source || s_next_seq == UINT32_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_message_entry_t replay = *source;
    replay.seq = s_next_seq++;
    s_entries[s_head] = replay;
    s_head = (s_head + 1U) % D1L_MESSAGE_STORE_CAPACITY;
    if (s_count < D1L_MESSAGE_STORE_CAPACITY) {
        s_count++;
    }
    return ESP_OK;
}

static esp_err_t adopt_primary_with_live_overlay_locked(
    const d1l_message_store_blob_t *primary)
{
    size_t overlay_count = 0U;
    fill_blob(&s_blob_scratch);
    const size_t oldest = blob_oldest_index(s_blob_scratch.head,
                                            s_blob_scratch.count);
    for (size_t i = 0U; i < s_blob_scratch.count; ++i) {
        const d1l_message_entry_t *entry =
            &s_blob_scratch.entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        if (entry->seq >= s_boot_next_seq &&
            overlay_count < D1L_MESSAGE_STORE_CAPACITY) {
            s_merge_scratch[overlay_count++] = *entry;
        }
    }
    const uint32_t overlay_mutations = s_mutations_since_init;
    load_blob_into_ram(primary);
    for (size_t i = 0U; i < overlay_count; ++i) {
        const esp_err_t replay_ret =
            append_replayed_entry_locked(&s_merge_scratch[i]);
        if (replay_ret != ESP_OK) {
            load_blob_into_ram(&s_blob_scratch);
            return replay_ret;
        }
    }
    s_total_written = s_total_written > UINT32_MAX - overlay_mutations ?
        UINT32_MAX : s_total_written + overlay_mutations;
    if (s_total_written < s_count) {
        s_total_written = (uint32_t)s_count;
    }
    s_content_revision = s_content_revision > UINT32_MAX - overlay_mutations ?
        UINT32_MAX : s_content_revision + overlay_mutations;
    s_dropped_oldest = s_total_written >= s_count ?
        s_total_written - (uint32_t)s_count : 0U;
    return ESP_OK;
}

static bool sd_backend_generation_matches(uint32_t expected_generation)
{
    d1l_retained_blob_store_backend_state_t state = {0};
    return d1l_retained_blob_store_backend_state(D1L_MESSAGE_STORE_ID,
                                                  &state) &&
           state.enabled && state.generation == expected_generation;
}

static esp_err_t note_sd_reconcile_failure(esp_err_t failure)
{
    d1l_store_lock_take(&s_store_lock);
    s_sd_reconcile_pending = true;
    s_sd_primary_last_error = failure;
    s_sd_primary_fail_count++;
    s_persistence_fail_count++;
    s_retry_pending = true;
    s_persistence_dirty = true;
    d1l_store_lock_give(&s_store_lock);
    return failure;
}

static esp_err_t reconcile_sd_primary(uint32_t expected_generation)
{
    if (!sd_backend_generation_matches(expected_generation)) {
        return note_sd_reconcile_failure(ESP_ERR_INVALID_STATE);
    }
    bool found = false;
    bool migrated = false;
    const esp_err_t read_ret = read_blob_target(true, &s_sd_blob_scratch,
                                                &found, &migrated);
    if (read_ret == ESP_ERR_NOT_FINISHED) {
        return read_ret;
    }
    if (read_ret != ESP_OK && read_ret != ESP_ERR_NOT_FOUND) {
        return note_sd_reconcile_failure(read_ret);
    }
    if (!sd_backend_generation_matches(expected_generation)) {
        return note_sd_reconcile_failure(ESP_ERR_INVALID_STATE);
    }

    d1l_store_lock_take(&s_store_lock);
    if (!found) {
        s_sd_reconcile_pending = false;
        s_sd_primary_last_error = ESP_OK;
        s_sd_primary_dirty = s_total_written > 0U || s_epoch > 1U ||
                             s_clear_lineage != 0U;
        s_persistence_dirty = persistence_dirty_locked(true);
        d1l_store_lock_give(&s_store_lock);
        return ESP_OK;
    }

    /* The backend can change again after the post-read check. Keep an exact
     * local rollback point until the merge and a final generation sample both
     * succeed; stale removable state must never become the NVS snapshot. */
    fill_blob(&s_persist_snapshot.blob);
    bool ram_changed = false;
    esp_err_t merge_ret = ESP_OK;
    if (s_device_lineage_authoritative &&
        s_sd_blob_scratch.clear_lineage != s_clear_lineage) {
        /* Device-local clear lineage wins. The foreign card will be rewritten
         * from the local snapshot after the guarded generation check. */
    } else if (!s_device_lineage_authoritative &&
               s_sd_blob_scratch.clear_lineage != s_clear_lineage) {
        merge_ret = adopt_primary_with_live_overlay_locked(
            &s_sd_blob_scratch);
        ram_changed = merge_ret == ESP_OK;
    } else if (s_sd_blob_scratch.epoch > s_epoch && s_mutated_since_init) {
        merge_ret = adopt_primary_with_live_overlay_locked(
            &s_sd_blob_scratch);
        ram_changed = merge_ret == ESP_OK;
    } else if (s_sd_blob_scratch.epoch > s_epoch) {
        load_blob_into_ram(&s_sd_blob_scratch);
        ram_changed = true;
    } else if (s_sd_blob_scratch.epoch < s_epoch) {
        /* A newer local clear/data epoch dominates stale or replacement SD. */
    } else {
        merge_ret = merge_blob_into_ram_locked(&s_sd_blob_scratch,
                                               &ram_changed);
    }
    if (merge_ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        return note_sd_reconcile_failure(merge_ret);
    }
    if (!sd_backend_generation_matches(expected_generation)) {
        load_blob_into_ram(&s_persist_snapshot.blob);
        d1l_store_lock_give(&s_store_lock);
        return note_sd_reconcile_failure(ESP_ERR_INVALID_STATE);
    }
    if (!s_device_lineage_authoritative) {
        /* This is the only path that may seed lineage from removable media:
         * no valid NVS state and no local mutation/clear existed. */
        s_device_lineage_authoritative = true;
    }

    fill_blob(&s_blob_scratch);
    s_sd_primary_dirty = migrated ||
        !blobs_equivalent(&s_blob_scratch, &s_sd_blob_scratch);
    if (ram_changed) {
        s_revision++;
        s_nvs_fallback_dirty = true;
    }
    s_sd_reconcile_pending = false;
    s_sd_primary_last_error = ESP_OK;
    s_persistence_dirty = persistence_dirty_locked(true);
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

static esp_err_t pending_error_locked(void)
{
    if ((s_sd_primary_dirty || s_sd_reconcile_pending) &&
        s_sd_primary_last_error != ESP_OK) {
        return s_sd_primary_last_error;
    }
    if (s_nvs_fallback_dirty && s_nvs_fallback_last_error != ESP_OK) {
        return s_nvs_fallback_last_error;
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t persist_store(bool force)
{
    d1l_store_lock_take(&s_persist_io_lock);
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    if (!d1l_retained_blob_store_backend_state(D1L_MESSAGE_STORE_ID,
                                               &backend_state)) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool sd_required = backend_state.enabled;
    const uint32_t generation = backend_state.generation;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    d1l_store_lock_take(&s_store_lock);
    if (!s_loaded) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (generation != s_last_sd_backend_generation) {
        s_last_sd_backend_generation = generation;
        s_sd_reconcile_pending = true;
    }
    s_persistence_dirty = persistence_dirty_locked(sd_required);
    if (!s_persistence_dirty) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }
    const bool retry_ready = !s_persist_attempted_this_boot ||
        now_ms - s_last_persist_attempt_ms >=
            D1L_MESSAGE_STORE_PERSIST_RETRY_MS;
    if (s_retry_pending && !retry_ready) {
        const esp_err_t pending = pending_error_locked();
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return force ? pending : ESP_OK;
    }
    const bool reconcile_attempt = sd_required && s_sd_reconcile_pending;
    s_persist_attempted_this_boot = true;
    s_last_persist_attempt_ms = now_ms;
    d1l_store_lock_give(&s_store_lock);

    if (reconcile_attempt) {
        const esp_err_t reconcile_ret = reconcile_sd_primary(generation);
        if (reconcile_ret != ESP_OK) {
            if (reconcile_ret == ESP_ERR_NOT_FINISHED) {
                d1l_store_lock_give(&s_persist_io_lock);
                return reconcile_ret;
            }
            d1l_store_lock_take(&s_store_lock);
            const bool nvs_attempt = s_nvs_fallback_dirty;
            if (nvs_attempt) {
                fill_blob(&s_persist_snapshot.blob);
                s_persist_snapshot.revision = s_revision;
            }
            d1l_store_lock_give(&s_store_lock);

            if (nvs_attempt) {
                const esp_err_t nvs_ret =
                    d1l_retained_blob_store_write_nvs_fallback(
                        D1L_MESSAGE_STORE_ID, D1L_MESSAGE_STORE_KEY,
                        &s_persist_snapshot.blob,
                        sizeof(s_persist_snapshot.blob));
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
                s_persistence_dirty = persistence_dirty_locked(sd_required);
                s_retry_pending = true;
                d1l_store_lock_give(&s_store_lock);
            }
            d1l_store_lock_give(&s_persist_io_lock);
            return reconcile_ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    s_persistence_dirty = persistence_dirty_locked(sd_required);
    if (!s_persistence_dirty) {
        s_retry_pending = false;
        if (!s_sd_primary_dirty && !s_sd_reconcile_pending) {
            s_mutated_since_init = false;
            s_mutations_since_init = 0U;
            s_boot_next_seq = s_next_seq;
        }
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }
    fill_blob(&s_persist_snapshot.blob);
    s_persist_snapshot.revision = s_revision;
    s_persist_snapshot.sd_attempt = sd_required && s_sd_primary_dirty;
    s_persist_snapshot.nvs_attempt = s_nvs_fallback_dirty;
    d1l_store_lock_give(&s_store_lock);

    esp_err_t sd_ret = ESP_OK;
    esp_err_t nvs_ret = ESP_OK;
    if (s_persist_snapshot.sd_attempt) {
        if (!sd_backend_generation_matches(generation)) {
            sd_ret = ESP_ERR_INVALID_STATE;
        } else {
            sd_ret = d1l_retained_blob_store_write_sd_primary_guarded(
                D1L_MESSAGE_STORE_ID, D1L_MESSAGE_STORE_KEY,
                &s_persist_snapshot.blob, sizeof(s_persist_snapshot.blob),
                generation);
            if (sd_ret == ESP_ERR_NOT_FINISHED) {
                d1l_store_lock_give(&s_persist_io_lock);
                return sd_ret;
            }
        }
    }
    if (s_persist_snapshot.nvs_attempt) {
        nvs_ret = d1l_retained_blob_store_write_nvs_fallback(
            D1L_MESSAGE_STORE_ID, D1L_MESSAGE_STORE_KEY,
            &s_persist_snapshot.blob, sizeof(s_persist_snapshot.blob));
    }
    const bool generation_changed = s_persist_snapshot.sd_attempt &&
        !sd_backend_generation_matches(generation);
    if (generation_changed) {
        sd_ret = ESP_ERR_INVALID_STATE;
    }

    d1l_store_lock_take(&s_store_lock);
    const bool same_revision = s_revision == s_persist_snapshot.revision;
    if (s_persist_snapshot.sd_attempt) {
        s_sd_primary_last_error = sd_ret;
        if (generation_changed) {
            s_sd_reconcile_pending = true;
        }
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
    if (sd_ret != ESP_OK || nvs_ret != ESP_OK) {
        s_persistence_fail_count++;
    }
    s_persistence_dirty = persistence_dirty_locked(sd_required);
    if (s_persistence_dirty &&
        (sd_ret != ESP_OK || nvs_ret != ESP_OK || !same_revision)) {
        s_retry_pending = true;
    }
    if (!s_persistence_dirty) {
        s_retry_pending = false;
        if (!s_sd_primary_dirty && !s_sd_reconcile_pending) {
            s_mutated_since_init = false;
            s_mutations_since_init = 0U;
            s_boot_next_seq = s_next_seq;
        }
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

esp_err_t d1l_message_store_init(void)
{
    d1l_store_lock_take(&s_persist_io_lock);
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    if (!d1l_retained_blob_store_backend_state(D1L_MESSAGE_STORE_ID,
                                               &backend_state)) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }

    d1l_store_lock_take(&s_store_lock);
    clear_ram();
    reset_persistence_state(backend_state.generation);
    s_loaded = false;
    d1l_store_lock_give(&s_store_lock);

    bool nvs_found = false;
    bool nvs_migrated = false;
    const esp_err_t nvs_ret = read_blob_target(false, &s_nvs_blob_scratch,
                                               &nvs_found, &nvs_migrated);
    bool sd_found = false;
    bool sd_migrated = false;
    esp_err_t sd_ret = ESP_ERR_NOT_FOUND;
    if (backend_state.enabled) {
        sd_ret = read_blob_target(true, &s_sd_blob_scratch,
                                  &sd_found, &sd_migrated);
        if (!sd_backend_generation_matches(backend_state.generation)) {
            sd_ret = ESP_ERR_INVALID_STATE;
            sd_found = false;
        }
    }

    const bool nvs_problem = nvs_ret != ESP_OK && nvs_ret != ESP_ERR_NOT_FOUND;
    bool effective_sd_problem = backend_state.enabled &&
        sd_ret != ESP_OK && sd_ret != ESP_ERR_NOT_FOUND;
    const bool no_valid_source_with_error = !nvs_found && !sd_found &&
        (nvs_problem || effective_sd_problem);
    if (no_valid_source_with_error) {
        d1l_store_lock_take(&s_store_lock);
        if (nvs_problem) {
            s_nvs_fallback_last_error = nvs_ret;
            s_nvs_fallback_fail_count++;
            s_nvs_fallback_dirty = true;
        }
        if (effective_sd_problem) {
            s_sd_primary_last_error = sd_ret;
            s_sd_primary_fail_count++;
            s_sd_reconcile_pending = true;
        }
        s_persistence_fail_count++;
        s_retry_pending = true;
        s_persistence_dirty = persistence_dirty_locked(backend_state.enabled);
        s_loaded = false;
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return nvs_problem ? nvs_ret : sd_ret;
    }

    d1l_store_lock_take(&s_store_lock);
    if (nvs_found && sd_found) {
        const d1l_message_store_blob_t *selected = NULL;
        if (s_sd_blob_scratch.clear_lineage !=
            s_nvs_blob_scratch.clear_lineage) {
            selected = &s_nvs_blob_scratch;
        } else {
            selected = blob_is_newer(&s_sd_blob_scratch,
                                     &s_nvs_blob_scratch) ?
                &s_sd_blob_scratch : &s_nvs_blob_scratch;
        }
        const d1l_message_store_blob_t *other =
            selected == &s_sd_blob_scratch ?
            &s_nvs_blob_scratch : &s_sd_blob_scratch;
        load_blob_into_ram(selected);
        bool changed = false;
        const esp_err_t merge_ret = merge_blob_into_ram_locked(other, &changed);
        if (merge_ret != ESP_OK) {
            load_blob_into_ram(&s_nvs_blob_scratch);
            s_sd_reconcile_pending = true;
            s_sd_primary_last_error = merge_ret;
            s_sd_primary_fail_count++;
            s_persistence_fail_count++;
            s_retry_pending = true;
        }
    } else if (nvs_found) {
        load_blob_into_ram(&s_nvs_blob_scratch);
    } else if (sd_found) {
        load_blob_into_ram(&s_sd_blob_scratch);
    }
    s_device_lineage_authoritative = nvs_found || sd_found;

    if (backend_state.enabled &&
        !sd_backend_generation_matches(backend_state.generation)) {
        sd_ret = ESP_ERR_INVALID_STATE;
        effective_sd_problem = true;
        if (nvs_found) {
            load_blob_into_ram(&s_nvs_blob_scratch);
            s_device_lineage_authoritative = true;
        } else {
            clear_ram();
            s_epoch = 1U;
            s_content_revision = 1U;
            s_clear_lineage = 0U;
            s_device_lineage_authoritative = false;
            s_sd_reconcile_pending = true;
            s_sd_primary_last_error = sd_ret;
            s_sd_primary_fail_count++;
            s_persistence_fail_count++;
            s_retry_pending = true;
            s_loaded = false;
            d1l_store_lock_give(&s_store_lock);
            d1l_store_lock_give(&s_persist_io_lock);
            return sd_ret;
        }
    }

    fill_blob(&s_blob_scratch);
    const bool has_state = s_total_written > 0U || s_count > 0U ||
                           s_epoch > 1U || s_clear_lineage != 0U;
    s_nvs_fallback_dirty = nvs_migrated ||
        (sd_found && (!nvs_found ||
                      !blobs_equivalent(&s_blob_scratch,
                                        &s_nvs_blob_scratch)));
    if (backend_state.enabled && !effective_sd_problem &&
        !s_sd_reconcile_pending) {
        s_sd_primary_dirty = sd_migrated || (sd_found ?
            !blobs_equivalent(&s_blob_scratch, &s_sd_blob_scratch) :
            has_state);
    }
    if (effective_sd_problem) {
        s_sd_reconcile_pending = true;
        s_sd_primary_last_error = sd_ret;
        s_sd_primary_fail_count++;
        s_persistence_fail_count++;
        s_retry_pending = true;
    }
    if (nvs_problem) {
        s_nvs_fallback_last_error = nvs_ret;
        s_nvs_fallback_fail_count++;
        s_nvs_fallback_dirty = true;
        s_persistence_fail_count++;
        s_retry_pending = true;
    }
    if (!backend_state.enabled) {
        s_sd_reconcile_pending = true;
    }
    s_revision = 1U;
    s_boot_next_seq = s_next_seq;
    s_mutated_since_init = false;
    s_mutations_since_init = 0U;
    s_persistence_dirty = persistence_dirty_locked(backend_state.enabled);
    s_loaded = true;
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    return ESP_OK;
}

static esp_err_t ensure_store_initialized(void)
{
    d1l_store_lock_take(&s_init_recovery_lock);
    d1l_store_lock_take(&s_store_lock);
    const bool loaded = s_loaded;
    d1l_store_lock_give(&s_store_lock);
    if (loaded) {
        d1l_store_lock_give(&s_init_recovery_lock);
        return ESP_OK;
    }
    const esp_err_t ret = d1l_message_store_init();
    d1l_store_lock_take(&s_store_lock);
    const bool fallback_loaded = s_loaded;
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_init_recovery_lock);
    return fallback_loaded ? ESP_OK : ret;
}

esp_err_t d1l_message_store_clear(void)
{
    const esp_err_t init_ret = ensure_store_initialized();
    if (init_ret != ESP_OK) {
        return init_ret;
    }
    d1l_store_lock_take(&s_store_lock);
    const uint64_t lineage = new_clear_lineage();
    const uint32_t preserved_next_seq = s_next_seq;
    clear_ram();
    /* Sequence IDs are shared by every channel and must never rewind while
     * channel read cursors survive in their own retained store. Reusing one
     * after a full history clear would make a later message look already read. */
    s_next_seq = preserved_next_seq;
    s_epoch = s_epoch == UINT32_MAX ? 1U : s_epoch + 1U;
    s_clear_lineage = lineage;
    s_content_revision = s_content_revision == UINT32_MAX ?
        UINT32_MAX : s_content_revision + 1U;
    s_revision++;
    s_loaded = true;
    s_mutated_since_init = true;
    s_device_lineage_authoritative = true;
    s_mutations_since_init = 0U;
    s_boot_next_seq = preserved_next_seq;
    s_sd_primary_dirty = true;
    s_nvs_fallback_dirty = true;
    s_persistence_dirty = true;
    d1l_store_lock_give(&s_store_lock);
    const esp_err_t retire_ret =
        d1l_retained_blob_store_erase_nvs_fallback(
            D1L_MESSAGE_STORE_ID, D1L_MESSAGE_STORE_KEY);
    if (retire_ret != ESP_OK) {
        return retire_ret;
    }
    return d1l_message_store_flush();
}

esp_err_t d1l_message_store_clear_channel(uint64_t channel_id)
{
    if (channel_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t init_ret = ensure_store_initialized();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    d1l_store_lock_take(&s_store_lock);
    const size_t oldest = s_count == 0U ? 0U :
        (s_head + D1L_MESSAGE_STORE_CAPACITY - s_count) %
            D1L_MESSAGE_STORE_CAPACITY;
    size_t kept = 0U;
    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_message_entry_t *entry =
            &s_entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        if (entry->channel_id != channel_id) {
            s_merge_scratch[kept++] = *entry;
        }
    }
    memset(s_entries, 0, sizeof(s_entries));
    if (kept > 0U) {
        memcpy(s_entries, s_merge_scratch,
               kept * sizeof(s_entries[0]));
    }
    memset(s_merge_scratch, 0, sizeof(s_merge_scratch));
    s_count = kept;
    s_head = kept % D1L_MESSAGE_STORE_CAPACITY;
    if (s_volatile_valid && s_volatile_entry.channel_id == channel_id) {
        memset(&s_volatile_entry, 0, sizeof(s_volatile_entry));
        s_volatile_valid = false;
    }

    /* Rotate the global merge lineage even when the local projection already
     * has no matching rows. A late/stale SD copy must not resurrect history
     * explicitly cleared from exactly one channel. Other channel rows remain
     * in the authoritative new lineage and global sequence never rewinds. */
    s_epoch = s_epoch == UINT32_MAX ? 1U : s_epoch + 1U;
    s_clear_lineage = new_clear_lineage();
    s_content_revision = s_content_revision == UINT32_MAX ?
        UINT32_MAX : s_content_revision + 1U;
    s_revision++;
    s_loaded = true;
    s_mutated_since_init = true;
    s_device_lineage_authoritative = true;
    s_mutations_since_init = 0U;
    s_boot_next_seq = s_next_seq;
    s_sd_primary_dirty = true;
    s_nvs_fallback_dirty = true;
    s_persistence_dirty = true;
    d1l_store_lock_give(&s_store_lock);
    const esp_err_t retire_ret =
        d1l_retained_blob_store_erase_nvs_fallback(
            D1L_MESSAGE_STORE_ID, D1L_MESSAGE_STORE_KEY);
    if (retire_ret != ESP_OK) {
        return retire_ret;
    }
    return d1l_message_store_flush();
}

esp_err_t d1l_message_store_flush(void)
{
    const esp_err_t ret = ensure_store_initialized();
    return ret == ESP_OK ? persist_store(true) : ret;
}

esp_err_t d1l_message_store_flush_if_due(void)
{
    const esp_err_t ret = ensure_store_initialized();
    return ret == ESP_OK ? persist_store(false) : ret;
}

static esp_err_t append_channel_internal(
    uint64_t channel_id, const char *direction, const char *author,
    const char *text, int rssi_dbm, int snr_tenths,
    uint8_t path_hash_bytes, uint8_t path_hops, bool delivered, bool persist,
    bool defer_flush, uint32_t *out_seq)
{
    if (out_seq) {
        *out_seq = 0U;
    }
    if (channel_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const d1l_user_text_info_t text_info = d1l_user_text_validate(text);
    if (text_info.result != D1L_USER_TEXT_OK) {
        return text_info.result == D1L_USER_TEXT_TOO_LONG ?
            ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
    }
    if (!defer_flush) {
        const esp_err_t init_ret = ensure_store_initialized();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }

    if (defer_flush) {
        if (!d1l_store_lock_try_take(&s_store_lock)) {
            return ESP_ERR_TIMEOUT;
        }
        if (!s_loaded) {
            d1l_store_lock_give(&s_store_lock);
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        d1l_store_lock_take(&s_store_lock);
    }
    d1l_message_entry_t entry = {
        .seq = s_next_seq,
        .uptime_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
        .rssi_dbm = rssi_dbm,
        .snr_tenths = snr_tenths,
        .path_hash_bytes = path_hash_bytes,
        .path_hops = path_hops,
        .delivered = delivered,
        .channel_id = channel_id,
    };
    sanitize_ascii(entry.direction, sizeof(entry.direction), direction ? direction : "rx");
    sanitize_ascii(entry.author, sizeof(entry.author), author ? author : "Public");
    memcpy(entry.text, text, text_info.byte_count + 1U);
    if (entry.direction[0] == '\0') {
        strncpy(entry.direction, "rx", sizeof(entry.direction) - 1U);
    }
    if (entry.author[0] == '\0') {
        strncpy(entry.author, "Public", sizeof(entry.author) - 1U);
    }

    if (!persist) {
        /* The volatile preview borrows the next durable row.  Keeping it
         * outside the ring means a full history cannot be evicted merely by
         * asking the UI to refresh. */
        s_volatile_entry = entry;
        s_volatile_valid = true;
        if (out_seq) {
            *out_seq = entry.seq;
        }
        d1l_store_lock_give(&s_store_lock);
        return ESP_OK;
    }

    /* A real append owns the borrowed sequence and supersedes the volatile preview. */
    if (s_next_seq == UINT32_MAX || s_total_written == UINT32_MAX ||
        (s_count == D1L_MESSAGE_STORE_CAPACITY &&
         s_dropped_oldest == UINT32_MAX)) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_volatile_valid = false;
    s_next_seq++;

    s_entries[s_head] = entry;
    s_head = (s_head + 1U) % D1L_MESSAGE_STORE_CAPACITY;
    if (s_count < D1L_MESSAGE_STORE_CAPACITY) {
        s_count++;
    } else {
        s_dropped_oldest++;
    }
    s_total_written++;
    if (s_content_revision < UINT32_MAX) {
        s_content_revision++;
    }
    s_revision++;
    s_mutated_since_init = true;
    s_device_lineage_authoritative = true;
    if (s_mutations_since_init < UINT32_MAX) {
        s_mutations_since_init++;
    }
    s_sd_primary_dirty = true;
    s_nvs_fallback_dirty = true;
    s_persistence_dirty = true;
    if (out_seq) {
        *out_seq = entry.seq;
    }
    d1l_store_lock_give(&s_store_lock);
    return defer_flush ? ESP_OK : d1l_message_store_flush();
}

esp_err_t d1l_message_store_append_public(const char *direction, const char *author,
                                          const char *text, int rssi_dbm, int snr_tenths,
                                          uint8_t path_hash_bytes, uint8_t path_hops,
                                          bool delivered)
{
    return d1l_message_store_append_channel(
        D1L_CHANNEL_PUBLIC_ID, direction, author, text, rssi_dbm, snr_tenths,
        path_hash_bytes, path_hops, delivered, NULL);
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
esp_err_t d1l_message_store_append_public_volatile(const char *direction, const char *author,
                                                   const char *text, int rssi_dbm, int snr_tenths,
                                                   uint8_t path_hash_bytes, uint8_t path_hops,
                                                   bool delivered)
{
    return d1l_message_store_append_channel_volatile(
        D1L_CHANNEL_PUBLIC_ID, direction, author, text, rssi_dbm, snr_tenths,
        path_hash_bytes, path_hops, delivered);
}
#endif

esp_err_t d1l_message_store_append_channel(
    uint64_t channel_id, const char *direction, const char *author,
    const char *text, int rssi_dbm, int snr_tenths,
    uint8_t path_hash_bytes, uint8_t path_hops, bool delivered,
    uint32_t *out_seq)
{
    return append_channel_internal(
        channel_id, direction, author, text, rssi_dbm, snr_tenths,
        path_hash_bytes, path_hops, delivered, true, false, out_seq);
}

esp_err_t d1l_message_store_append_channel_deferred(
    uint64_t channel_id, const char *direction, const char *author,
    const char *text, int rssi_dbm, int snr_tenths,
    uint8_t path_hash_bytes, uint8_t path_hops, bool delivered,
    uint32_t *out_seq)
{
    if (!out_seq) {
        return ESP_ERR_INVALID_ARG;
    }
    return append_channel_internal(
        channel_id, direction, author, text, rssi_dbm, snr_tenths,
        path_hash_bytes, path_hops, delivered, true, true, out_seq);
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
esp_err_t d1l_message_store_append_channel_volatile(
    uint64_t channel_id, const char *direction, const char *author,
    const char *text, int rssi_dbm, int snr_tenths,
    uint8_t path_hash_bytes, uint8_t path_hops, bool delivered)
{
    return append_channel_internal(
        channel_id, direction, author, text, rssi_dbm, snr_tenths,
        path_hash_bytes, path_hops, delivered, false, false, NULL);
}
#endif

static d1l_message_store_stats_t message_store_stats_locked(
    d1l_retained_blob_store_backend_state_t backend_state)
{
    /*
     * A clean store still has work when the removable-SD backend generation
     * changes.  Reporting that drift lets the retained worker enter
     * persist_store(), where the generation is accepted and reconciled under
     * the persistence I/O lock.
     */
    const bool backend_generation_changed =
        s_loaded && backend_state.enabled &&
        backend_state.generation != s_last_sd_backend_generation;
    const bool reconcile_pending =
        backend_state.enabled &&
        (s_sd_reconcile_pending || backend_generation_changed);
    s_persistence_dirty =
        persistence_dirty_locked(backend_state.enabled) ||
        backend_generation_changed;
    size_t public_count = 0U;
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[i].channel_id == D1L_CHANNEL_PUBLIC_ID) {
            public_count++;
        }
    }
    d1l_message_store_stats_t stats = {
        .next_seq = s_next_seq,
        .total_written = s_total_written,
        .dropped_oldest = s_dropped_oldest,
        .epoch = s_epoch,
        .content_revision = s_content_revision,
        .clear_lineage = s_clear_lineage,
        .persistence_commit_count = s_persistence_commit_count,
        .persistence_fail_count = s_persistence_fail_count,
        .persistence_stale_snapshot_count = s_persistence_stale_snapshot_count,
        .sd_primary_commit_count = s_sd_primary_commit_count,
        .sd_primary_fail_count = s_sd_primary_fail_count,
        .sd_backend_generation = backend_state.generation,
        .sd_primary_last_error = s_sd_primary_last_error,
        .nvs_fallback_commit_count = s_nvs_fallback_commit_count,
        .nvs_fallback_fail_count = s_nvs_fallback_fail_count,
        .nvs_fallback_last_error = s_nvs_fallback_last_error,
        .persistence_revision = s_revision,
        .count = s_count,
        .public_count = public_count,
        .capacity = D1L_MESSAGE_STORE_CAPACITY,
        .loaded = s_loaded,
        .persistence_dirty = s_persistence_dirty,
        .sd_primary_required = backend_state.enabled,
        .sd_primary_dirty = s_sd_primary_dirty,
        .sd_primary_reconcile_pending = reconcile_pending,
        .nvs_fallback_dirty = s_nvs_fallback_dirty,
    };
    return stats;
}

d1l_message_store_stats_t d1l_message_store_stats(void)
{
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    (void)d1l_retained_blob_store_backend_state(D1L_MESSAGE_STORE_ID,
                                                &backend_state);
    d1l_store_lock_take(&s_store_lock);
    const d1l_message_store_stats_t stats =
        message_store_stats_locked(backend_state);
    d1l_store_lock_give(&s_store_lock);
    return stats;
}

esp_err_t d1l_message_store_snapshot_retained(
    d1l_message_entry_t *out_entries, size_t max_entries, size_t *out_count,
    d1l_message_retained_snapshot_t *out_snapshot)
{
    if (!out_entries || max_entries == 0U || !out_count || !out_snapshot) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0U;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    const esp_err_t init_ret = ensure_store_initialized();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    d1l_store_lock_take(&s_store_lock);
    if (max_entries < s_count) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t oldest = s_count == 0U ? 0U :
        (s_head + D1L_MESSAGE_STORE_CAPACITY - s_count) %
            D1L_MESSAGE_STORE_CAPACITY;
    for (size_t i = 0U; i < s_count; ++i) {
        out_entries[i] =
            s_entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
    }
    *out_count = s_count;
    *out_snapshot = (d1l_message_retained_snapshot_t){
        .epoch = s_epoch,
        .next_seq = s_next_seq,
        .clear_lineage = s_clear_lineage,
        .persistence_revision = s_revision,
    };
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

size_t d1l_message_store_copy_channel_recent(
    uint64_t channel_id, d1l_message_entry_t *out_entries, size_t max_entries)
{
    if (channel_id == 0U || out_entries == NULL || max_entries == 0U) {
        return 0;
    }

    d1l_store_lock_take(&s_store_lock);
    if (s_count == 0 && !s_volatile_valid) {
        d1l_store_lock_give(&s_store_lock);
        return 0;
    }
    const size_t oldest = s_count == 0 ? 0 :
        (s_head + D1L_MESSAGE_STORE_CAPACITY - s_count) % D1L_MESSAGE_STORE_CAPACITY;
    size_t channel_count = 0U;
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY].channel_id ==
            channel_id) {
            channel_count++;
        }
    }
    if (s_volatile_valid && s_volatile_entry.channel_id == channel_id) {
        channel_count++;
    }
    const size_t copied = channel_count < max_entries ?
        channel_count : max_entries;
    const size_t first_match = channel_count - copied;
    size_t match_index = 0U;
    size_t out_index = 0U;
    for (size_t i = 0U; i < s_count && out_index < copied; ++i) {
        const d1l_message_entry_t *entry =
            &s_entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        if (entry->channel_id != channel_id) {
            continue;
        }
        if (match_index++ >= first_match) {
            out_entries[out_index++] = *entry;
        }
    }
    if (s_volatile_valid && s_volatile_entry.channel_id == channel_id &&
        out_index < copied && match_index >= first_match) {
        out_entries[out_index++] = s_volatile_entry;
    }
    d1l_store_lock_give(&s_store_lock);
    return out_index;
}

size_t d1l_message_store_copy_recent(d1l_message_entry_t *out_entries,
                                     size_t max_entries)
{
    return d1l_message_store_copy_channel_recent(
        D1L_CHANNEL_PUBLIC_ID, out_entries, max_entries);
}

size_t d1l_message_store_query_channel_page(
    uint64_t channel_id, d1l_message_entry_t *out_entries, size_t max_entries,
    size_t skip_newest, const char *query, size_t *out_total_matches)
{
    if (out_total_matches) {
        *out_total_matches = 0;
    }
    if (channel_id == 0U || out_entries == NULL || max_entries == 0U) {
        return 0;
    }

    d1l_store_lock_take(&s_store_lock);
    if (s_count == 0 && !s_volatile_valid) {
        d1l_store_lock_give(&s_store_lock);
        return 0;
    }
    size_t total_matches = 0;
    const size_t oldest = s_count == 0 ? 0 :
        (s_head + D1L_MESSAGE_STORE_CAPACITY - s_count) % D1L_MESSAGE_STORE_CAPACITY;
    for (size_t i = 0; i < s_count; ++i) {
        const d1l_message_entry_t *entry = &s_entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        if (entry->channel_id != channel_id ||
            !message_matches_query(entry, query)) {
            continue;
        }
        total_matches++;
    }
    if (s_volatile_valid && s_volatile_entry.channel_id == channel_id &&
        message_matches_query(&s_volatile_entry, query)) {
        total_matches++;
    }

    if (out_total_matches) {
        *out_total_matches = total_matches;
    }
    if (skip_newest >= total_matches) {
        d1l_store_lock_give(&s_store_lock);
        return 0;
    }

    const size_t available = total_matches - skip_newest;
    const size_t copied = available < max_entries ? available : max_entries;
    const size_t first_match = total_matches - skip_newest - copied;
    const size_t last_match = first_match + copied;
    size_t match_index = 0;
    size_t out_index = 0;
    for (size_t i = 0; i < s_count && out_index < copied; ++i) {
        const d1l_message_entry_t *entry = &s_entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        if (entry->channel_id != channel_id ||
            !message_matches_query(entry, query)) {
            continue;
        }
        if (match_index >= first_match && match_index < last_match) {
            out_entries[out_index++] = *entry;
        }
        match_index++;
    }
    if (s_volatile_valid && s_volatile_entry.channel_id == channel_id &&
        out_index < copied &&
        message_matches_query(&s_volatile_entry, query)) {
        if (match_index >= first_match && match_index < last_match) {
            out_entries[out_index++] = s_volatile_entry;
        }
    }
    d1l_store_lock_give(&s_store_lock);
    return out_index;
}

size_t d1l_message_store_query_page(d1l_message_entry_t *out_entries,
                                    size_t max_entries, size_t skip_newest,
                                    const char *query,
                                    size_t *out_total_matches)
{
    return d1l_message_store_query_channel_page(
        D1L_CHANNEL_PUBLIC_ID, out_entries, max_entries, skip_newest, query,
        out_total_matches);
}

size_t d1l_message_store_query_page_snapshot(
    d1l_message_entry_t *out_entries, size_t max_entries,
    size_t skip_newest, const char *query, size_t *out_total_matches,
    d1l_message_store_stats_t *out_stats, uint32_t *out_volatile_seq)
{
    if (out_total_matches) {
        *out_total_matches = 0U;
    }
    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
    }
    if (out_volatile_seq) {
        *out_volatile_seq = 0U;
    }
    if (!out_entries || max_entries == 0U || !out_stats ||
        !out_volatile_seq) {
        return 0U;
    }

    d1l_retained_blob_store_backend_state_t backend_state = {0};
    (void)d1l_retained_blob_store_backend_state(D1L_MESSAGE_STORE_ID,
                                                &backend_state);
    d1l_store_lock_take(&s_store_lock);
    size_t total_matches = 0U;
    const size_t oldest = s_count == 0U ? 0U :
        (s_head + D1L_MESSAGE_STORE_CAPACITY - s_count) %
            D1L_MESSAGE_STORE_CAPACITY;
    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_message_entry_t *entry =
            &s_entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
        if (entry->channel_id == D1L_CHANNEL_PUBLIC_ID &&
            message_matches_query(entry, query)) {
            total_matches++;
        }
    }
    const bool volatile_matches =
        s_volatile_valid &&
        s_volatile_entry.channel_id == D1L_CHANNEL_PUBLIC_ID &&
        message_matches_query(&s_volatile_entry, query);
    if (volatile_matches) {
        total_matches++;
        *out_volatile_seq = s_volatile_entry.seq;
    }
    if (out_total_matches) {
        *out_total_matches = total_matches;
    }

    size_t copied = 0U;
    if (skip_newest < total_matches) {
        const size_t available = total_matches - skip_newest;
        copied = available < max_entries ? available : max_entries;
        const size_t first_match =
            total_matches - skip_newest - copied;
        const size_t last_match = first_match + copied;
        size_t match_index = 0U;
        for (size_t i = 0U; i < s_count && copied > 0U; ++i) {
            const d1l_message_entry_t *entry =
                &s_entries[(oldest + i) % D1L_MESSAGE_STORE_CAPACITY];
            if (entry->channel_id != D1L_CHANNEL_PUBLIC_ID ||
                !message_matches_query(entry, query)) {
                continue;
            }
            if (match_index >= first_match && match_index < last_match) {
                out_entries[match_index - first_match] = *entry;
            }
            match_index++;
        }
        if (volatile_matches && match_index >= first_match &&
            match_index < last_match) {
            out_entries[match_index - first_match] = s_volatile_entry;
        }
    }
    *out_stats = message_store_stats_locked(backend_state);
    d1l_store_lock_give(&s_store_lock);
    return copied;
}

size_t d1l_message_store_query(d1l_message_entry_t *out_entries, size_t max_entries,
                               const char *query)
{
    return d1l_message_store_query_page(out_entries, max_entries, 0, query, NULL);
}

size_t d1l_message_store_query_channel(
    uint64_t channel_id, d1l_message_entry_t *out_entries,
    size_t max_entries, const char *query)
{
    return d1l_message_store_query_channel_page(
        channel_id, out_entries, max_entries, 0U, query, NULL);
}
