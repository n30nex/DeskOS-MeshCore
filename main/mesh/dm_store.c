#include "dm_store.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "mesh/store_lock.h"
#include "storage/retained_blob_store.h"

#define D1L_DM_STORE_ID D1L_RETAINED_BLOB_STORE_DM_MESSAGES
#define D1L_DM_STORE_KEY "threads"
#define D1L_DM_STORE_SCHEMA 7U
#define D1L_DM_STORE_SCHEMA_V6 6U
#define D1L_DM_STORE_SCHEMA_V5 5U
#define D1L_DM_STORE_SCHEMA_V4 4U
#define D1L_DM_STORE_SCHEMA_V3 3U
#define D1L_DM_STORE_SCHEMA_V2 2U
#define D1L_DM_STORE_SCHEMA_V1 1U
#define D1L_DM_TEXT_LEN_V1 96U
#define D1L_DM_CONTACT_ALIAS_LEN_LEGACY 24U
#define D1L_DM_CONTACT_ALIAS_LEN_V6_WIDE 32U

_Static_assert(D1L_DM_CONTACT_ALIAS_LEN ==
                   D1L_DM_CONTACT_ALIAS_LEN_V6_WIDE,
               "schema-v7 DM alias width must remain frozen");

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
    d1l_dm_entry_t entries[D1L_DM_STORE_CAPACITY];
} d1l_dm_store_blob_t;

/* Frozen schema-v2/v3/v4 entry layout. Never alias a legacy blob through the
 * live d1l_dm_entry_t: extending the live row must not silently change the
 * accepted legacy byte sizes. */
typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    char contact_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char contact_alias[D1L_DM_CONTACT_ALIAS_LEN_LEGACY];
    char direction[D1L_DM_DIRECTION_LEN];
    char text[D1L_MESSAGE_TEXT_LEN];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint8_t attempt;
    bool delivered;
    bool acked;
    uint32_t ack_hash;
} d1l_dm_entry_v4_t;

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
    d1l_dm_entry_v4_t entries[D1L_DM_STORE_CAPACITY];
} d1l_dm_store_blob_v4_t;

/* Schema v5 added durable inbound ACK-dispatch metadata.  Keep its exact
 * entry layout frozen now that schema v6 extends the live row with outbound
 * delivery state. */
typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    char contact_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char contact_alias[D1L_DM_CONTACT_ALIAS_LEN_LEGACY];
    char direction[D1L_DM_DIRECTION_LEN];
    char text[D1L_MESSAGE_TEXT_LEN];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint8_t attempt;
    bool delivered;
    bool acked;
    uint32_t ack_hash;
    uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    uint8_t ack_dispatch_count;
    uint8_t ack_dispatch_kind;
    d1l_dm_ack_state_t ack_state;
    esp_err_t ack_last_error;
    bool identity_digest_valid;
} d1l_dm_entry_v5_t;

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
    d1l_dm_entry_v5_t entries[D1L_DM_STORE_CAPACITY];
} d1l_dm_store_blob_v5_t;

/* Schema v6 was emitted with two different entry ABIs.  The first writer used
 * the historical 24-byte contact alias; a later contact-only change widened
 * the shared alias macro to 32 without advancing the DM schema.  Preserve both
 * exact layouts and distinguish them only by their complete byte length. */
typedef struct {
    uint32_t seq;
    uint64_t delivery_session_id;
    uint32_t uptime_ms;
    char contact_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char contact_alias[D1L_DM_CONTACT_ALIAS_LEN_LEGACY];
    char direction[D1L_DM_DIRECTION_LEN];
    char text[D1L_MESSAGE_TEXT_LEN];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint8_t attempt;
    bool delivered;
    bool acked;
    uint32_t ack_hash;
    uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    uint8_t ack_dispatch_count;
    uint8_t ack_dispatch_kind;
    d1l_dm_ack_state_t ack_state;
    esp_err_t ack_last_error;
    bool identity_digest_valid;
    d1l_dm_delivery_state_t delivery_state;
    d1l_dm_delivery_reason_t delivery_reason;
    esp_err_t delivery_last_error;
    uint8_t delivery_retry_count;
    uint32_t delivery_revision;
} d1l_dm_entry_v6_narrow_t;

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
    d1l_dm_entry_v6_narrow_t entries[D1L_DM_STORE_CAPACITY];
} d1l_dm_store_blob_v6_narrow_t;

/* The second schema-v6 ABI is byte-for-byte the current 32-byte alias layout.
 * Schema v7 changes only the schema discriminator, not this representation. */
typedef d1l_dm_store_blob_t d1l_dm_store_blob_v6_wide_t;

_Static_assert(sizeof(d1l_dm_store_blob_v6_narrow_t) !=
                   sizeof(d1l_dm_store_blob_v6_wide_t),
               "schema-v6 DM ABI lengths must remain distinguishable");

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    uint32_t epoch;
    uint32_t content_revision;
    d1l_dm_entry_v4_t entries[D1L_DM_STORE_CAPACITY];
} d1l_dm_store_blob_v3_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    d1l_dm_entry_v4_t entries[D1L_DM_STORE_CAPACITY];
} d1l_dm_store_blob_v2_t;

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    char contact_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char contact_alias[D1L_DM_CONTACT_ALIAS_LEN_LEGACY];
    char direction[D1L_DM_DIRECTION_LEN];
    char text[D1L_DM_TEXT_LEN_V1];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint8_t attempt;
    bool delivered;
    bool acked;
    uint32_t ack_hash;
} d1l_dm_entry_v1_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    d1l_dm_entry_v1_t entries[D1L_DM_STORE_CAPACITY];
} d1l_dm_store_blob_v1_t;

typedef union {
    uint32_t schema;
    d1l_dm_store_blob_t v7;
    d1l_dm_store_blob_v6_wide_t v6_wide;
    d1l_dm_store_blob_v6_narrow_t v6_narrow;
    d1l_dm_store_blob_v5_t v5;
    d1l_dm_store_blob_v4_t v4;
    d1l_dm_store_blob_v3_t v3;
    d1l_dm_store_blob_v2_t v2;
    d1l_dm_store_blob_v1_t v1;
} d1l_dm_store_raw_blob_t;

typedef struct {
    uint64_t state_revision;
    bool sd_attempt;
    bool nvs_attempt;
    d1l_dm_store_blob_t blob;
} d1l_dm_persist_snapshot_t;

typedef struct {
    bool active;
    uint64_t delivery_session_id;
    uint32_t previous_revision;
    uint32_t target_revision;
    d1l_dm_delivery_state_t previous_state;
    d1l_dm_delivery_reason_t previous_reason;
    esp_err_t previous_error;
} d1l_dm_ack_persistence_pending_t;

static d1l_dm_entry_t s_entries[D1L_DM_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static size_t s_head;
static size_t s_count;
static uint32_t s_next_seq = 1U;
static uint32_t s_total_written;
static uint32_t s_dropped_oldest;
static uint32_t s_epoch = 1U;
static uint32_t s_content_revision = 1U;
static uint64_t s_clear_lineage;
static d1l_dm_entry_t s_volatile_entry;
static bool s_volatile_valid;
static bool s_loaded;

static bool s_sd_primary_dirty;
static bool s_sd_reconcile_pending;
static bool s_nvs_fallback_dirty;
static bool s_last_sd_primary_required;
static uint32_t s_last_sd_backend_generation;
static bool s_retry_pending;
static bool s_persist_attempted_this_boot;
static uint32_t s_last_persist_attempt_ms;
static uint32_t s_boot_next_seq = 1U;
static bool s_mutated_since_init;
static uint32_t s_mutations_since_init;
static bool s_device_lineage_authoritative;
static uint64_t s_state_revision;
static uint32_t s_persistence_commit_count;
static uint32_t s_persistence_fail_count;
static uint32_t s_persistence_stale_snapshot_count;
static uint32_t s_sd_primary_commit_count;
static uint32_t s_sd_primary_fail_count;
static esp_err_t s_sd_primary_last_error;
static uint32_t s_nvs_fallback_commit_count;
static uint32_t s_nvs_fallback_fail_count;
static esp_err_t s_nvs_fallback_last_error;
static d1l_dm_ack_persistence_pending_t
    s_ack_persistence_pending[D1L_DM_STORE_CAPACITY];

static d1l_dm_store_raw_blob_t s_raw_scratch EXT_RAM_BSS_ATTR;
static d1l_dm_store_blob_t s_sd_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_dm_store_blob_t s_nvs_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_dm_store_blob_t s_compare_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_dm_persist_snapshot_t s_persist_snapshot EXT_RAM_BSS_ATTR;
static d1l_dm_entry_t s_reconcile_volatile_scratch EXT_RAM_BSS_ATTR;
static d1l_dm_entry_t s_merge_entries[D1L_DM_STORE_CAPACITY * 2U]
    EXT_RAM_BSS_ATTR;
static d1l_dm_entry_t s_overlay_entries[D1L_DM_STORE_CAPACITY]
    EXT_RAM_BSS_ATTR;
static d1l_store_lock_t s_store_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_persist_io_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_init_recovery_lock = D1L_STORE_LOCK_INITIALIZER;

static esp_err_t persist_store(bool force);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static unsigned char ascii_lower(unsigned char value)
{
    return value >= (unsigned char)'A' && value <= (unsigned char)'Z' ?
        (unsigned char)(value + ((unsigned char)'a' - (unsigned char)'A')) :
        value;
}

/* Deliberately folds ASCII only. UTF-8 continuation and leading bytes retain
 * their exact values, so the scan cannot invent Unicode equivalence. */
static bool contains_ascii_casefold(const char *haystack, const char *needle)
{
    if (!needle || needle[0] == '\0') {
        return true;
    }
    if (!haystack) {
        return false;
    }
    for (size_t i = 0U; haystack[i] != '\0'; ++i) {
        size_t j = 0U;
        while (needle[j] != '\0' && haystack[i + j] != '\0' &&
               ascii_lower((unsigned char)haystack[i + j]) ==
                   ascii_lower((unsigned char)needle[j])) {
            j++;
        }
        if (needle[j] == '\0') {
            return true;
        }
    }
    return false;
}

static bool dm_entry_matches_query(const d1l_dm_entry_t *entry,
                                   const char *query)
{
    return entry &&
        (!query || query[0] == '\0' ||
         contains_ascii_casefold(entry->contact_alias, query) ||
         contains_ascii_casefold(entry->contact_fingerprint, query) ||
         contains_ascii_casefold(entry->text, query) ||
         contains_ascii_casefold(entry->direction, query));
}

static d1l_dm_ack_persistence_pending_t *find_ack_persistence_pending_locked(
    uint64_t delivery_session_id, uint32_t target_revision)
{
    for (size_t i = 0U; i < D1L_DM_STORE_CAPACITY; ++i) {
        d1l_dm_ack_persistence_pending_t *pending =
            &s_ack_persistence_pending[i];
        if (pending->active &&
            pending->delivery_session_id == delivery_session_id &&
            (target_revision == 0U ||
             pending->target_revision == target_revision)) {
            return pending;
        }
    }
    return NULL;
}

static d1l_dm_ack_persistence_pending_t *reserve_ack_persistence_pending_locked(
    const d1l_dm_entry_t *entry)
{
    if (!entry || entry->delivery_session_id == 0U ||
        entry->delivery_revision == UINT32_MAX) {
        return NULL;
    }
    d1l_dm_ack_persistence_pending_t *available = NULL;
    for (size_t i = 0U; i < D1L_DM_STORE_CAPACITY; ++i) {
        d1l_dm_ack_persistence_pending_t *pending =
            &s_ack_persistence_pending[i];
        if (pending->active &&
            pending->delivery_session_id == entry->delivery_session_id) {
            return NULL;
        }
        if (!pending->active && !available) {
            available = pending;
        }
    }
    if (!available) {
        return NULL;
    }
    *available = (d1l_dm_ack_persistence_pending_t) {
        .active = true,
        .delivery_session_id = entry->delivery_session_id,
        .previous_revision = entry->delivery_revision,
        .target_revision = entry->delivery_revision + 1U,
        .previous_state = entry->delivery_state,
        .previous_reason = entry->delivery_reason,
        .previous_error = entry->delivery_last_error,
    };
    return available;
}

static void project_ack_persistence_truth_locked(d1l_dm_entry_t *entry)
{
    if (!entry || entry->delivery_state != D1L_DM_DELIVERY_ACKNOWLEDGED) {
        return;
    }
    const d1l_dm_ack_persistence_pending_t *pending =
        find_ack_persistence_pending_locked(
            entry->delivery_session_id, entry->delivery_revision);
    if (!pending) {
        return;
    }
    entry->delivery_state = pending->previous_state;
    entry->delivery_reason = pending->previous_reason;
    entry->delivery_last_error = pending->previous_error;
    entry->delivery_revision = pending->previous_revision;
    entry->acked = false;
    entry->delivered = false;
}

static void acknowledge_persisted_delivery_truth_locked(void)
{
    memset(s_ack_persistence_pending, 0,
           sizeof(s_ack_persistence_pending));
}

static uint64_t fnv1a64_bytes(uint64_t hash, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0U; bytes && i < len; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t fnv1a64_u32_le(uint64_t hash, uint32_t value)
{
    const uint8_t bytes[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8U),
        (uint8_t)(value >> 16U),
        (uint8_t)(value >> 24U),
    };
    return fnv1a64_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t mix64(uint64_t value)
{
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static bool delivery_session_id_exists_locked(uint64_t session_id)
{
    if (session_id == 0U) {
        return true;
    }
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[i].delivery_session_id == session_id) {
            return true;
        }
    }
    return false;
}

static uint64_t new_delivery_session_id_locked(void)
{
    for (size_t attempt = 0U; attempt < 8U; ++attempt) {
        const uint64_t candidate = ((uint64_t)esp_random() << 32U) |
                                   (uint64_t)esp_random();
        if (!delivery_session_id_exists_locked(candidate)) {
            return candidate;
        }
    }
    const uint64_t seed = s_clear_lineage ^
        ((uint64_t)s_epoch << 32U) ^ (uint64_t)s_next_seq ^
        UINT64_C(0xd1d34e5e55100101);
    for (uint64_t probe = 0U; probe <= D1L_DM_STORE_CAPACITY; ++probe) {
        uint64_t candidate = mix64(seed + probe);
        if (candidate == 0U) {
            candidate = UINT64_C(0xd1d34e5e55100101) ^ probe;
        }
        if (!delivery_session_id_exists_locked(candidate)) {
            return candidate;
        }
    }
    return 0U;
}

static void sanitize_ascii(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0U) {
        return;
    }
    size_t out = 0U;
    while (src && src[0] && out + 1U < dest_size) {
        unsigned char c = (unsigned char)*src++;
        if (c < 32U || c > 126U || c == '"' || c == '\\') {
            c = '_';
        }
        dest[out++] = (char)c;
    }
    dest[out] = '\0';
}

static void sanitize_ascii_bounded(char *dest, size_t dest_size,
                                   const char *src, size_t src_size)
{
    if (!dest || dest_size == 0U) {
        return;
    }
    size_t out = 0U;
    for (size_t i = 0U; src && i < src_size && src[i] != '\0' &&
         out + 1U < dest_size; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c < 32U || c > 126U || c == '"' || c == '\\') {
            c = '_';
        }
        dest[out++] = (char)c;
    }
    dest[out] = '\0';
}

static bool persisted_ascii_is_valid(const char *text, size_t capacity,
                                     bool allow_empty)
{
    if (!text || capacity == 0U || memchr(text, '\0', capacity) == NULL ||
        (!allow_empty && text[0] == '\0')) {
        return false;
    }
    for (size_t i = 0U; i < capacity && text[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (c < 32U || c > 126U) {
            return false;
        }
    }
    return true;
}

static bool legacy_entry_is_valid(const d1l_dm_entry_v4_t *entry,
                                  uint32_t next_seq)
{
    return entry && entry->seq > 0U && entry->seq < next_seq &&
           persisted_ascii_is_valid(entry->contact_fingerprint,
                                    sizeof(entry->contact_fingerprint), false) &&
           persisted_ascii_is_valid(entry->contact_alias,
                                    sizeof(entry->contact_alias), false) &&
           persisted_ascii_is_valid(entry->direction,
                                    sizeof(entry->direction), false) &&
           persisted_ascii_is_valid(entry->text, sizeof(entry->text), false) &&
           entry->path_hash_bytes <= 3U && entry->path_hops <= 63U &&
           (uint16_t)entry->path_hash_bytes * entry->path_hops <= 64U;
}

static bool digest_bytes_are_zero(
    const uint8_t digest[D1L_DM_IDENTITY_DIGEST_BYTES])
{
    if (!digest) {
        return false;
    }
    for (size_t i = 0U; i < D1L_DM_IDENTITY_DIGEST_BYTES; ++i) {
        if (digest[i] != 0U) {
            return false;
        }
    }
    return true;
}

static bool valid_identity_digest_equal(const d1l_dm_entry_t *left,
                                        const d1l_dm_entry_t *right)
{
    return left && right && left->identity_digest_valid &&
           right->identity_digest_valid &&
           memcmp(left->identity_digest, right->identity_digest,
                  D1L_DM_IDENTITY_DIGEST_BYTES) == 0;
}

static bool ack_metadata_is_valid(const d1l_dm_entry_t *entry)
{
    if (!entry) {
        return false;
    }
    if (!entry->identity_digest_valid) {
        return entry->ack_dispatch_count == 0U &&
               entry->ack_dispatch_kind == 0U &&
               entry->ack_state == D1L_DM_ACK_STATE_LEGACY_UNVERIFIED &&
               entry->ack_last_error == ESP_OK &&
               digest_bytes_are_zero(entry->identity_digest);
    }
    if (strncmp(entry->direction, "rx", sizeof(entry->direction)) != 0 ||
        entry->ack_dispatch_count > D1L_DM_ACK_DISPATCH_MAX ||
        entry->ack_dispatch_kind > D1L_DM_ACK_DISPATCH_KIND_MAX ||
        entry->ack_state == D1L_DM_ACK_STATE_LEGACY_UNVERIFIED) {
        return false;
    }
    if (entry->ack_dispatch_count == 0U) {
        return entry->ack_state == D1L_DM_ACK_STATE_RETRYABLE &&
               entry->ack_dispatch_kind == 0U &&
               entry->ack_last_error == ESP_OK;
    }
    if (entry->ack_dispatch_kind == 0U) {
        return false;
    }
    switch (entry->ack_state) {
    case D1L_DM_ACK_STATE_PENDING:
    case D1L_DM_ACK_STATE_SENT:
        return entry->ack_last_error == ESP_OK;
    case D1L_DM_ACK_STATE_RETRYABLE:
        return entry->ack_dispatch_count == 1U &&
               entry->ack_last_error != ESP_OK;
    case D1L_DM_ACK_STATE_TERMINAL:
        return entry->ack_dispatch_count == D1L_DM_ACK_DISPATCH_MAX &&
               entry->ack_last_error != ESP_OK;
    default:
        return false;
    }
}

static bool delivery_state_is_failure(d1l_dm_delivery_state_t state)
{
    return state == D1L_DM_DELIVERY_FAILED_RADIO ||
           state == D1L_DM_DELIVERY_FAILED_TIMEOUT ||
           state == D1L_DM_DELIVERY_FAILED_QUEUE ||
           state == D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT;
}

static uint64_t migration_delivery_session_id(const d1l_dm_entry_t *entry)
{
    static const uint8_t domain[] = "SIGUI-DM-DELIVERY-MIGRATION-V1";
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = fnv1a64_bytes(hash, domain, sizeof(domain));
    hash = fnv1a64_bytes(hash, entry->contact_fingerprint,
                         strlen(entry->contact_fingerprint) + 1U);
    hash = fnv1a64_bytes(hash, entry->direction,
                         strlen(entry->direction) + 1U);
    hash = fnv1a64_bytes(hash, entry->text, strlen(entry->text) + 1U);
    hash = fnv1a64_u32_le(hash, entry->uptime_ms);
    hash = fnv1a64_u32_le(hash, entry->ack_hash);
    hash = mix64(hash);
    return hash == 0U ? UINT64_C(0xd1d34e5e55100001) : hash;
}

static bool delivery_reason_matches_target(
    d1l_dm_delivery_state_t state, d1l_dm_delivery_reason_t reason);

static bool delivery_metadata_is_valid(const d1l_dm_entry_t *entry)
{
    if (!entry || !d1l_dm_delivery_state_valid(entry->delivery_state) ||
        !d1l_dm_delivery_reason_valid(entry->delivery_reason)) {
        return false;
    }
    const bool outbound =
        strncmp(entry->direction, "tx", sizeof(entry->direction)) == 0;
    if (!outbound) {
        return entry->delivery_session_id == 0U &&
               entry->delivery_state == D1L_DM_DELIVERY_NOT_APPLICABLE &&
               entry->delivery_reason == D1L_DM_DELIVERY_REASON_NONE &&
               entry->delivery_last_error == ESP_OK &&
               entry->delivery_retry_count == 0U &&
               entry->delivery_revision == 0U;
    }
    const bool acknowledged =
        entry->delivery_state == D1L_DM_DELIVERY_ACKNOWLEDGED;
    if (entry->delivery_session_id == 0U ||
        entry->delivery_state == D1L_DM_DELIVERY_NOT_APPLICABLE ||
        entry->delivery_reason == D1L_DM_DELIVERY_REASON_NONE ||
        !delivery_reason_matches_target(entry->delivery_state,
                                        entry->delivery_reason) ||
        entry->delivery_revision == 0U ||
        entry->delivery_retry_count > entry->attempt ||
        entry->delivered != acknowledged || entry->acked != acknowledged) {
        return false;
    }
    return !delivery_state_is_failure(entry->delivery_state) ||
           entry->delivery_last_error != ESP_OK;
}

static bool v5_ack_metadata_is_valid(const d1l_dm_entry_v5_t *entry)
{
    if (!entry) {
        return false;
    }
    d1l_dm_entry_t current = {0};
    memcpy(current.direction, entry->direction, sizeof(current.direction));
    memcpy(current.identity_digest, entry->identity_digest,
           sizeof(current.identity_digest));
    current.ack_dispatch_count = entry->ack_dispatch_count;
    current.ack_dispatch_kind = entry->ack_dispatch_kind;
    current.ack_state = entry->ack_state;
    current.ack_last_error = entry->ack_last_error;
    current.identity_digest_valid = entry->identity_digest_valid;
    return ack_metadata_is_valid(&current);
}

static bool v5_entry_is_valid(const d1l_dm_entry_v5_t *entry,
                              uint32_t next_seq)
{
    return entry && entry->seq > 0U && entry->seq < next_seq &&
           persisted_ascii_is_valid(entry->contact_fingerprint,
                                    sizeof(entry->contact_fingerprint), false) &&
           persisted_ascii_is_valid(entry->contact_alias,
                                    sizeof(entry->contact_alias), false) &&
           persisted_ascii_is_valid(entry->direction,
                                    sizeof(entry->direction), false) &&
           persisted_ascii_is_valid(entry->text, sizeof(entry->text), false) &&
           entry->path_hash_bytes <= 3U && entry->path_hops <= 63U &&
           (uint16_t)entry->path_hash_bytes * entry->path_hops <= 64U &&
           v5_ack_metadata_is_valid(entry);
}

static bool persisted_entry_is_valid(const d1l_dm_entry_t *entry,
                                     uint32_t next_seq)
{
    return entry && entry->seq > 0U && entry->seq < next_seq &&
           persisted_ascii_is_valid(entry->contact_fingerprint,
                                    sizeof(entry->contact_fingerprint), false) &&
           persisted_ascii_is_valid(entry->contact_alias,
                                    sizeof(entry->contact_alias), false) &&
           persisted_ascii_is_valid(entry->direction,
                                    sizeof(entry->direction), false) &&
           d1l_user_text_validate_bounded(entry->text,
                                          sizeof(entry->text), false).result ==
               D1L_USER_TEXT_OK &&
           entry->path_hash_bytes <= 3U && entry->path_hops <= 63U &&
           (uint16_t)entry->path_hash_bytes * entry->path_hops <= 64U &&
           ack_metadata_is_valid(entry) &&
           delivery_metadata_is_valid(entry);
}

static void convert_v6_narrow_entry(const d1l_dm_entry_v6_narrow_t *old,
                                    d1l_dm_entry_t *out)
{
    memset(out, 0, sizeof(*out));
    out->seq = old->seq;
    out->delivery_session_id = old->delivery_session_id;
    out->uptime_ms = old->uptime_ms;
    memcpy(out->contact_fingerprint, old->contact_fingerprint,
           sizeof(old->contact_fingerprint));
    memcpy(out->contact_alias, old->contact_alias,
           sizeof(old->contact_alias));
    memcpy(out->direction, old->direction, sizeof(old->direction));
    memcpy(out->text, old->text, sizeof(old->text));
    out->rssi_dbm = old->rssi_dbm;
    out->snr_tenths = old->snr_tenths;
    out->path_hash_bytes = old->path_hash_bytes;
    out->path_hops = old->path_hops;
    out->attempt = old->attempt;
    out->delivered = old->delivered;
    out->acked = old->acked;
    out->ack_hash = old->ack_hash;
    memcpy(out->identity_digest, old->identity_digest,
           sizeof(old->identity_digest));
    out->ack_dispatch_count = old->ack_dispatch_count;
    out->ack_dispatch_kind = old->ack_dispatch_kind;
    out->ack_state = old->ack_state;
    out->ack_last_error = old->ack_last_error;
    out->identity_digest_valid = old->identity_digest_valid;
    out->delivery_state = old->delivery_state;
    out->delivery_reason = old->delivery_reason;
    out->delivery_last_error = old->delivery_last_error;
    out->delivery_retry_count = old->delivery_retry_count;
    out->delivery_revision = old->delivery_revision;
}

static bool blob_layout_is_valid(const d1l_dm_store_blob_t *blob, size_t len,
                                 uint32_t expected_schema)
{
    if (!blob || len != sizeof(*blob) || blob->schema != expected_schema ||
        blob->epoch == 0U || blob->content_revision == 0U ||
        blob->head >= D1L_DM_STORE_CAPACITY ||
        blob->count > D1L_DM_STORE_CAPACITY || blob->next_seq == 0U ||
        blob->total_written < blob->count ||
        blob->head != blob->count % D1L_DM_STORE_CAPACITY) {
        return false;
    }
    uint32_t previous_seq = 0U;
    for (size_t i = 0U; i < blob->count; ++i) {
        if (!persisted_entry_is_valid(&blob->entries[i], blob->next_seq) ||
            blob->entries[i].seq <= previous_seq) {
            return false;
        }
        for (size_t previous = 0U; previous < i; ++previous) {
            if (valid_identity_digest_equal(&blob->entries[previous],
                                            &blob->entries[i])) {
                return false;
            }
            if (blob->entries[i].delivery_session_id != 0U &&
                blob->entries[previous].delivery_session_id ==
                    blob->entries[i].delivery_session_id) {
                return false;
            }
        }
        previous_seq = blob->entries[i].seq;
    }
    return true;
}

static bool blob_is_valid(const d1l_dm_store_blob_t *blob, size_t len)
{
    return blob_layout_is_valid(blob, len, D1L_DM_STORE_SCHEMA);
}

static bool blob_v6_wide_is_valid(const d1l_dm_store_blob_v6_wide_t *blob,
                                  size_t len)
{
    return blob_layout_is_valid(blob, len, D1L_DM_STORE_SCHEMA_V6);
}

static bool blob_v6_narrow_is_valid(
    const d1l_dm_store_blob_v6_narrow_t *blob, size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_DM_STORE_SCHEMA_V6 || blob->epoch == 0U ||
        blob->content_revision == 0U ||
        blob->head >= D1L_DM_STORE_CAPACITY ||
        blob->count > D1L_DM_STORE_CAPACITY || blob->next_seq == 0U ||
        blob->total_written < blob->count ||
        blob->head != blob->count % D1L_DM_STORE_CAPACITY) {
        return false;
    }
    uint32_t previous_seq = 0U;
    for (size_t i = 0U; i < blob->count; ++i) {
        const d1l_dm_entry_v6_narrow_t *entry = &blob->entries[i];
        d1l_dm_entry_t current = {0};
        if (!persisted_ascii_is_valid(entry->contact_alias,
                                      sizeof(entry->contact_alias), false)) {
            return false;
        }
        convert_v6_narrow_entry(entry, &current);
        if (!persisted_entry_is_valid(&current, blob->next_seq) ||
            entry->seq <= previous_seq) {
            return false;
        }
        for (size_t previous = 0U; previous < i; ++previous) {
            if (entry->identity_digest_valid &&
                blob->entries[previous].identity_digest_valid &&
                memcmp(blob->entries[previous].identity_digest,
                       entry->identity_digest,
                       D1L_DM_IDENTITY_DIGEST_BYTES) == 0) {
                return false;
            }
            if (entry->delivery_session_id != 0U &&
                blob->entries[previous].delivery_session_id ==
                    entry->delivery_session_id) {
                return false;
            }
        }
        previous_seq = entry->seq;
    }
    return true;
}

static bool blob_v5_is_valid(const d1l_dm_store_blob_v5_t *blob, size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_DM_STORE_SCHEMA_V5 || blob->epoch == 0U ||
        blob->content_revision == 0U ||
        blob->head >= D1L_DM_STORE_CAPACITY ||
        blob->count > D1L_DM_STORE_CAPACITY || blob->next_seq == 0U ||
        blob->total_written < blob->count ||
        blob->head != blob->count % D1L_DM_STORE_CAPACITY) {
        return false;
    }
    uint32_t previous_seq = 0U;
    for (size_t i = 0U; i < blob->count; ++i) {
        const d1l_dm_entry_v5_t *entry = &blob->entries[i];
        if (!v5_entry_is_valid(entry, blob->next_seq) ||
            entry->seq <= previous_seq) {
            return false;
        }
        if (entry->identity_digest_valid) {
            for (size_t previous = 0U; previous < i; ++previous) {
                if (blob->entries[previous].identity_digest_valid &&
                    memcmp(blob->entries[previous].identity_digest,
                           entry->identity_digest,
                           D1L_DM_IDENTITY_DIGEST_BYTES) == 0) {
                    return false;
                }
            }
        }
        previous_seq = entry->seq;
    }
    return true;
}

static bool blob_v2_is_valid(const d1l_dm_store_blob_v2_t *blob,
                             size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_DM_STORE_SCHEMA_V2 ||
        blob->head >= D1L_DM_STORE_CAPACITY ||
        blob->count > D1L_DM_STORE_CAPACITY || blob->next_seq == 0U ||
        blob->total_written < blob->count) {
        return false;
    }
    const size_t oldest = blob->count == 0U ? 0U :
        (blob->head + D1L_DM_STORE_CAPACITY - blob->count) %
        D1L_DM_STORE_CAPACITY;
    uint32_t previous_seq = 0U;
    for (size_t i = 0U; i < blob->count; ++i) {
        const d1l_dm_entry_v4_t *entry =
            &blob->entries[(oldest + i) % D1L_DM_STORE_CAPACITY];
        if (!legacy_entry_is_valid(entry, blob->next_seq) ||
            entry->seq <= previous_seq) {
            return false;
        }
        previous_seq = entry->seq;
    }
    return true;
}

static bool blob_v4_is_valid(const d1l_dm_store_blob_v4_t *blob, size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_DM_STORE_SCHEMA_V4 || blob->epoch == 0U ||
        blob->content_revision == 0U ||
        blob->head >= D1L_DM_STORE_CAPACITY ||
        blob->count > D1L_DM_STORE_CAPACITY || blob->next_seq == 0U ||
        blob->total_written < blob->count ||
        blob->head != blob->count % D1L_DM_STORE_CAPACITY) {
        return false;
    }
    uint32_t previous_seq = 0U;
    for (size_t i = 0U; i < blob->count; ++i) {
        if (!legacy_entry_is_valid(&blob->entries[i], blob->next_seq) ||
            blob->entries[i].seq <= previous_seq) {
            return false;
        }
        previous_seq = blob->entries[i].seq;
    }
    return true;
}

static bool blob_v3_is_valid(const d1l_dm_store_blob_v3_t *blob, size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_DM_STORE_SCHEMA_V3 || blob->epoch == 0U ||
        blob->content_revision == 0U ||
        blob->head >= D1L_DM_STORE_CAPACITY ||
        blob->count > D1L_DM_STORE_CAPACITY || blob->next_seq == 0U ||
        blob->total_written < blob->count ||
        blob->head != blob->count % D1L_DM_STORE_CAPACITY) {
        return false;
    }
    uint32_t previous_seq = 0U;
    for (size_t i = 0U; i < blob->count; ++i) {
        if (!legacy_entry_is_valid(&blob->entries[i], blob->next_seq) ||
            blob->entries[i].seq <= previous_seq) {
            return false;
        }
        previous_seq = blob->entries[i].seq;
    }
    return true;
}

static bool blob_v1_header_is_valid(const d1l_dm_store_blob_v1_t *blob,
                                    size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_DM_STORE_SCHEMA_V1 &&
           blob->head < D1L_DM_STORE_CAPACITY &&
           blob->count <= D1L_DM_STORE_CAPACITY && blob->next_seq > 0U &&
           blob->total_written >= blob->count;
}

static uint32_t migration_revision(uint32_t total_written)
{
    return total_written == UINT32_MAX ? UINT32_MAX : total_written + 1U;
}

static void initialize_legacy_delivery_metadata(d1l_dm_entry_t *entry)
{
    if (!entry ||
        strncmp(entry->direction, "tx", sizeof(entry->direction)) != 0) {
        if (entry) {
            entry->delivery_session_id = 0U;
            entry->delivery_state = D1L_DM_DELIVERY_NOT_APPLICABLE;
            entry->delivery_reason = D1L_DM_DELIVERY_REASON_NONE;
            entry->delivery_last_error = ESP_OK;
            entry->delivery_retry_count = 0U;
            entry->delivery_revision = 0U;
        }
        return;
    }
    entry->delivery_session_id = migration_delivery_session_id(entry);
    entry->delivery_state = entry->acked ?
        D1L_DM_DELIVERY_ACKNOWLEDGED :
        D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT;
    entry->delivery_reason = D1L_DM_DELIVERY_REASON_LEGACY_IMPORT;
    entry->delivery_last_error = entry->acked ? ESP_OK :
        D1L_DM_DELIVERY_INTERRUPTED_ERROR;
    entry->delivery_retry_count = 0U;
    entry->delivery_revision = 1U;
    entry->acked = entry->delivery_state ==
        D1L_DM_DELIVERY_ACKNOWLEDGED;
    entry->delivered = entry->acked;
}

static void convert_v5_entry(const d1l_dm_entry_v5_t *old,
                             d1l_dm_entry_t *out)
{
    memset(out, 0, sizeof(*out));
    out->seq = old->seq;
    out->uptime_ms = old->uptime_ms;
    memcpy(out->contact_fingerprint, old->contact_fingerprint,
           sizeof(out->contact_fingerprint));
    memcpy(out->contact_alias, old->contact_alias,
           sizeof(old->contact_alias));
    memcpy(out->direction, old->direction, sizeof(out->direction));
    memcpy(out->text, old->text, sizeof(out->text));
    out->rssi_dbm = old->rssi_dbm;
    out->snr_tenths = old->snr_tenths;
    out->path_hash_bytes = old->path_hash_bytes;
    out->path_hops = old->path_hops;
    out->attempt = old->attempt;
    out->delivered = old->delivered;
    out->acked = old->acked;
    out->ack_hash = old->ack_hash;
    memcpy(out->identity_digest, old->identity_digest,
           sizeof(out->identity_digest));
    out->ack_dispatch_count = old->ack_dispatch_count;
    out->ack_dispatch_kind = old->ack_dispatch_kind;
    out->ack_state = old->ack_state;
    out->ack_last_error = old->ack_last_error;
    out->identity_digest_valid = old->identity_digest_valid;
    initialize_legacy_delivery_metadata(out);
}

static void convert_v5_blob(const d1l_dm_store_blob_v5_t *old,
                            d1l_dm_store_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    out->schema = D1L_DM_STORE_SCHEMA;
    out->epoch = old->epoch;
    out->content_revision = old->content_revision;
    out->clear_lineage = old->clear_lineage;
    out->next_seq = old->next_seq;
    out->total_written = old->total_written;
    out->dropped_oldest = old->dropped_oldest;
    out->head = old->count % D1L_DM_STORE_CAPACITY;
    out->count = old->count;
    const size_t oldest = old->count == 0U ? 0U :
        (old->head + D1L_DM_STORE_CAPACITY - old->count) %
        D1L_DM_STORE_CAPACITY;
    for (size_t i = 0U; i < old->count; ++i) {
        convert_v5_entry(&old->entries[
                             (oldest + i) % D1L_DM_STORE_CAPACITY],
                         &out->entries[i]);
    }
}

static void convert_v4_entry(const d1l_dm_entry_v4_t *old,
                             d1l_dm_entry_t *out)
{
    memset(out, 0, sizeof(*out));
    out->seq = old->seq;
    out->uptime_ms = old->uptime_ms;
    memcpy(out->contact_fingerprint, old->contact_fingerprint,
           sizeof(out->contact_fingerprint));
    memcpy(out->contact_alias, old->contact_alias,
           sizeof(old->contact_alias));
    memcpy(out->direction, old->direction, sizeof(out->direction));
    memcpy(out->text, old->text, sizeof(out->text));
    out->rssi_dbm = old->rssi_dbm;
    out->snr_tenths = old->snr_tenths;
    out->path_hash_bytes = old->path_hash_bytes;
    out->path_hops = old->path_hops;
    out->attempt = old->attempt;
    out->delivered = old->delivered;
    out->acked = old->acked;
    out->ack_hash = old->ack_hash;
    out->ack_state = D1L_DM_ACK_STATE_LEGACY_UNVERIFIED;
    out->ack_last_error = ESP_OK;
    initialize_legacy_delivery_metadata(out);
}

static void convert_v4_blob(const d1l_dm_store_blob_v4_t *old,
                            d1l_dm_store_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    out->schema = D1L_DM_STORE_SCHEMA;
    out->epoch = old->epoch;
    out->content_revision = old->content_revision;
    out->clear_lineage = old->clear_lineage;
    out->next_seq = old->next_seq;
    out->total_written = old->total_written;
    out->dropped_oldest = old->dropped_oldest;
    out->head = old->count % D1L_DM_STORE_CAPACITY;
    out->count = old->count;
    const size_t oldest = old->count == 0U ? 0U :
        (old->head + D1L_DM_STORE_CAPACITY - old->count) %
        D1L_DM_STORE_CAPACITY;
    for (size_t i = 0U; i < old->count; ++i) {
        convert_v4_entry(&old->entries[
                             (oldest + i) % D1L_DM_STORE_CAPACITY],
                         &out->entries[i]);
    }
}

static void convert_v3_blob(const d1l_dm_store_blob_v3_t *old,
                            d1l_dm_store_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    out->schema = D1L_DM_STORE_SCHEMA;
    out->epoch = old->epoch;
    out->content_revision = old->content_revision;
    out->clear_lineage = 0U;
    out->next_seq = old->next_seq;
    out->total_written = old->total_written;
    out->dropped_oldest = old->dropped_oldest;
    out->head = old->count % D1L_DM_STORE_CAPACITY;
    out->count = old->count;
    const size_t oldest = old->count == 0U ? 0U :
        (old->head + D1L_DM_STORE_CAPACITY - old->count) %
        D1L_DM_STORE_CAPACITY;
    for (size_t i = 0U; i < old->count; ++i) {
        convert_v4_entry(&old->entries[
                             (oldest + i) % D1L_DM_STORE_CAPACITY],
                         &out->entries[i]);
    }
}

static void convert_v2_blob(const d1l_dm_store_blob_v2_t *old,
                            d1l_dm_store_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    out->schema = D1L_DM_STORE_SCHEMA;
    out->epoch = 1U;
    out->content_revision = migration_revision(old->total_written);
    out->clear_lineage = 0U;
    out->next_seq = old->next_seq;
    out->total_written = old->total_written;
    out->dropped_oldest = old->dropped_oldest;
    out->head = old->count % D1L_DM_STORE_CAPACITY;
    out->count = old->count;
    const size_t oldest = old->count == 0U ? 0U :
        (old->head + D1L_DM_STORE_CAPACITY - old->count) % D1L_DM_STORE_CAPACITY;
    for (size_t i = 0U; i < old->count; ++i) {
        convert_v4_entry(&old->entries[
                             (oldest + i) % D1L_DM_STORE_CAPACITY],
                         &out->entries[i]);
    }
}

static void convert_v1_blob(const d1l_dm_store_blob_v1_t *old,
                            d1l_dm_store_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    out->schema = D1L_DM_STORE_SCHEMA;
    out->epoch = 1U;
    out->content_revision = migration_revision(old->total_written);
    out->clear_lineage = 0U;
    out->next_seq = old->next_seq;
    out->total_written = old->total_written;
    out->dropped_oldest = old->dropped_oldest;
    out->head = old->count % D1L_DM_STORE_CAPACITY;
    out->count = old->count;
    const size_t oldest = old->count == 0U ? 0U :
        (old->head + D1L_DM_STORE_CAPACITY - old->count) % D1L_DM_STORE_CAPACITY;
    for (size_t i = 0U; i < old->count; ++i) {
        const d1l_dm_entry_v1_t *source =
            &old->entries[(oldest + i) % D1L_DM_STORE_CAPACITY];
        d1l_dm_entry_t *dest = &out->entries[i];
        dest->seq = source->seq;
        dest->uptime_ms = source->uptime_ms;
        sanitize_ascii_bounded(dest->contact_fingerprint,
                               sizeof(dest->contact_fingerprint),
                               source->contact_fingerprint,
                               sizeof(source->contact_fingerprint));
        sanitize_ascii_bounded(dest->contact_alias,
                               sizeof(dest->contact_alias),
                               source->contact_alias,
                               sizeof(source->contact_alias));
        sanitize_ascii_bounded(dest->direction, sizeof(dest->direction),
                               source->direction, sizeof(source->direction));
        sanitize_ascii_bounded(dest->text, sizeof(dest->text), source->text,
                               sizeof(source->text));
        dest->rssi_dbm = source->rssi_dbm;
        dest->snr_tenths = source->snr_tenths;
        dest->path_hash_bytes = source->path_hash_bytes;
        dest->path_hops = source->path_hops;
        dest->attempt = source->attempt;
        dest->delivered = source->delivered;
        dest->acked = source->acked;
        dest->ack_hash = source->ack_hash;
        dest->ack_state = D1L_DM_ACK_STATE_LEGACY_UNVERIFIED;
        dest->ack_last_error = ESP_OK;
        initialize_legacy_delivery_metadata(dest);
    }
}

static void convert_v6_narrow_blob(const d1l_dm_store_blob_v6_narrow_t *old,
                                   d1l_dm_store_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    out->schema = D1L_DM_STORE_SCHEMA;
    out->epoch = old->epoch;
    out->content_revision = old->content_revision;
    out->clear_lineage = old->clear_lineage;
    out->next_seq = old->next_seq;
    out->total_written = old->total_written;
    out->dropped_oldest = old->dropped_oldest;
    out->head = old->count % D1L_DM_STORE_CAPACITY;
    out->count = old->count;
    for (size_t i = 0U; i < old->count; ++i) {
        convert_v6_narrow_entry(&old->entries[i], &out->entries[i]);
    }
}

static void convert_v6_wide_blob(const d1l_dm_store_blob_v6_wide_t *old,
                                 d1l_dm_store_blob_t *out)
{
    *out = *old;
    out->schema = D1L_DM_STORE_SCHEMA;
}

static esp_err_t decode_raw_blob(const d1l_dm_store_raw_blob_t *raw, size_t len,
                                 d1l_dm_store_blob_t *out, bool *out_upgrade)
{
    if (!raw || !out || !out_upgrade || len < sizeof(raw->schema)) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_upgrade = false;
    if (raw->schema == D1L_DM_STORE_SCHEMA) {
        if (!blob_is_valid(&raw->v7, len)) {
            return ESP_ERR_INVALID_STATE;
        }
        *out = raw->v7;
        return ESP_OK;
    }
    if (raw->schema == D1L_DM_STORE_SCHEMA_V6) {
        if (len == sizeof(raw->v6_narrow)) {
            if (!blob_v6_narrow_is_valid(&raw->v6_narrow, len)) {
                return ESP_ERR_INVALID_STATE;
            }
            convert_v6_narrow_blob(&raw->v6_narrow, out);
        } else if (len == sizeof(raw->v6_wide)) {
            if (!blob_v6_wide_is_valid(&raw->v6_wide, len)) {
                return ESP_ERR_INVALID_STATE;
            }
            convert_v6_wide_blob(&raw->v6_wide, out);
        } else {
            return ESP_ERR_INVALID_STATE;
        }
        if (!blob_is_valid(out, sizeof(*out))) {
            return ESP_ERR_INVALID_STATE;
        }
        *out_upgrade = true;
        return ESP_OK;
    }
    if (raw->schema == D1L_DM_STORE_SCHEMA_V5) {
        if (!blob_v5_is_valid(&raw->v5, len)) {
            return ESP_ERR_INVALID_STATE;
        }
        convert_v5_blob(&raw->v5, out);
        if (!blob_is_valid(out, sizeof(*out))) {
            return ESP_ERR_INVALID_STATE;
        }
        *out_upgrade = true;
        return ESP_OK;
    }
    if (raw->schema == D1L_DM_STORE_SCHEMA_V4) {
        if (!blob_v4_is_valid(&raw->v4, len)) {
            return ESP_ERR_INVALID_STATE;
        }
        convert_v4_blob(&raw->v4, out);
        if (!blob_is_valid(out, sizeof(*out))) {
            return ESP_ERR_INVALID_STATE;
        }
        *out_upgrade = true;
        return ESP_OK;
    }
    if (raw->schema == D1L_DM_STORE_SCHEMA_V3) {
        if (!blob_v3_is_valid(&raw->v3, len)) {
            return ESP_ERR_INVALID_STATE;
        }
        convert_v3_blob(&raw->v3, out);
        if (!blob_is_valid(out, sizeof(*out))) {
            return ESP_ERR_INVALID_STATE;
        }
        *out_upgrade = true;
        return ESP_OK;
    }
    if (raw->schema == D1L_DM_STORE_SCHEMA_V2) {
        if (!blob_v2_is_valid(&raw->v2, len)) {
            return ESP_ERR_INVALID_STATE;
        }
        convert_v2_blob(&raw->v2, out);
        if (!blob_is_valid(out, sizeof(*out))) {
            return ESP_ERR_INVALID_STATE;
        }
        *out_upgrade = true;
        return ESP_OK;
    }
    if (raw->schema == D1L_DM_STORE_SCHEMA_V1) {
        if (!blob_v1_header_is_valid(&raw->v1, len)) {
            return ESP_ERR_INVALID_STATE;
        }
        convert_v1_blob(&raw->v1, out);
        if (!blob_is_valid(out, sizeof(*out))) {
            return ESP_ERR_INVALID_STATE;
        }
        *out_upgrade = true;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t read_decoded_blob(bool sd_primary, d1l_dm_store_blob_t *out,
                                   bool *out_upgrade)
{
    memset(&s_raw_scratch, 0, sizeof(s_raw_scratch));
    size_t len = sizeof(s_raw_scratch);
    const esp_err_t ret = sd_primary ?
        d1l_retained_blob_store_read_sd_primary(
            D1L_DM_STORE_ID, D1L_DM_STORE_KEY, &s_raw_scratch, &len) :
        d1l_retained_blob_store_read_nvs_fallback(
            D1L_DM_STORE_ID, D1L_DM_STORE_KEY, &s_raw_scratch, &len);
    if (ret != ESP_OK) {
        return ret;
    }
    return decode_raw_blob(&s_raw_scratch, len, out, out_upgrade);
}

static void clear_history_locked(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    acknowledge_persisted_delivery_truth_locked();
    s_head = 0U;
    s_count = 0U;
    s_next_seq = 1U;
    s_total_written = 0U;
    s_dropped_oldest = 0U;
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

static void reset_store_locked(bool sd_primary_required,
                               uint32_t backend_generation)
{
    clear_history_locked();
    s_epoch = 1U;
    s_content_revision = 1U;
    s_clear_lineage = 0U;
    s_loaded = false;
    s_sd_primary_dirty = false;
    s_sd_reconcile_pending = !sd_primary_required;
    s_nvs_fallback_dirty = false;
    s_last_sd_primary_required = sd_primary_required;
    s_last_sd_backend_generation = backend_generation;
    s_retry_pending = false;
    s_persist_attempted_this_boot = false;
    s_last_persist_attempt_ms = 0U;
    s_boot_next_seq = 1U;
    s_mutated_since_init = false;
    s_mutations_since_init = 0U;
    s_device_lineage_authoritative = false;
    s_state_revision = 1U;
    s_persistence_commit_count = 0U;
    s_persistence_fail_count = 0U;
    s_persistence_stale_snapshot_count = 0U;
    s_sd_primary_commit_count = 0U;
    s_sd_primary_fail_count = 0U;
    s_sd_primary_last_error = ESP_OK;
    s_nvs_fallback_commit_count = 0U;
    s_nvs_fallback_fail_count = 0U;
    s_nvs_fallback_last_error = ESP_OK;
}

static void fill_blob_locked(d1l_dm_store_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->schema = D1L_DM_STORE_SCHEMA;
    blob->epoch = s_epoch;
    blob->content_revision = s_content_revision;
    blob->clear_lineage = s_clear_lineage;
    blob->next_seq = s_next_seq;
    blob->total_written = s_total_written;
    blob->dropped_oldest = s_dropped_oldest;
    blob->head = s_count % D1L_DM_STORE_CAPACITY;
    const size_t oldest = s_count == 0U ? 0U :
        (s_head + D1L_DM_STORE_CAPACITY - s_count) % D1L_DM_STORE_CAPACITY;
    for (size_t i = 0U; i < s_count; ++i) {
        blob->entries[blob->count++] =
            s_entries[(oldest + i) % D1L_DM_STORE_CAPACITY];
    }
}

static void load_blob_locked(const d1l_dm_store_blob_t *blob)
{
    clear_history_locked();
    s_epoch = blob->epoch;
    s_content_revision = blob->content_revision;
    s_clear_lineage = blob->clear_lineage;
    s_next_seq = blob->next_seq;
    s_total_written = blob->total_written;
    s_dropped_oldest = blob->dropped_oldest;
    s_count = blob->count;
    memcpy(s_entries, blob->entries, blob->count * sizeof(blob->entries[0]));
    s_head = blob->count % D1L_DM_STORE_CAPACITY;
}

static bool normalize_interrupted_ack_locked(void)
{
    bool changed = false;
    for (size_t i = 0U; i < s_count; ++i) {
        d1l_dm_entry_t *entry = &s_entries[i];
        if (!entry->identity_digest_valid ||
            entry->ack_state != D1L_DM_ACK_STATE_PENDING) {
            continue;
        }
        entry->ack_state =
            entry->ack_dispatch_count >= D1L_DM_ACK_DISPATCH_MAX ?
                D1L_DM_ACK_STATE_TERMINAL : D1L_DM_ACK_STATE_RETRYABLE;
        entry->ack_last_error = D1L_DM_ACK_INTERRUPTED_ERROR;
        changed = true;
    }
    if (changed) {
        if (s_content_revision < UINT32_MAX) {
            s_content_revision++;
        }
        s_state_revision++;
    }
    return changed;
}

static bool normalize_interrupted_delivery_locked(void)
{
    bool changed = false;
    for (size_t i = 0U; i < s_count; ++i) {
        d1l_dm_entry_t *entry = &s_entries[i];
        if (strncmp(entry->direction, "tx", sizeof(entry->direction)) != 0 ||
            !d1l_dm_delivery_interrupted_by_reboot(entry->delivery_state)) {
            continue;
        }
        entry->delivery_state = D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT;
        entry->delivery_reason = D1L_DM_DELIVERY_REASON_REBOOT_RECOVERY;
        entry->delivery_last_error = D1L_DM_DELIVERY_INTERRUPTED_ERROR;
        entry->acked = false;
        entry->delivered = false;
        if (entry->delivery_revision < UINT32_MAX) {
            entry->delivery_revision++;
        }
        changed = true;
    }
    if (changed) {
        if (s_content_revision < UINT32_MAX) {
            s_content_revision++;
        }
        s_state_revision++;
    }
    return changed;
}

static bool blobs_equal(const d1l_dm_store_blob_t *left,
                        const d1l_dm_store_blob_t *right)
{
    return left && right && memcmp(left, right, sizeof(*left)) == 0;
}

static bool outbound_delivery_identity_material_equal(
    const d1l_dm_entry_t *left, const d1l_dm_entry_t *right)
{
    return left && right && left->uptime_ms == right->uptime_ms &&
           left->ack_hash == right->ack_hash &&
           strncmp(left->contact_fingerprint, right->contact_fingerprint,
                   sizeof(left->contact_fingerprint)) == 0 &&
           strncmp(left->direction, right->direction,
                   sizeof(left->direction)) == 0 &&
           strncmp(left->text, right->text, sizeof(left->text)) == 0;
}

static bool delivery_session_id_conflicts(const d1l_dm_entry_t *left,
                                          const d1l_dm_entry_t *right)
{
    return left && right && left->delivery_session_id != 0U &&
           left->delivery_session_id == right->delivery_session_id &&
           !outbound_delivery_identity_material_equal(left, right);
}

static bool entry_identity_equal(const d1l_dm_entry_t *left,
                                 const d1l_dm_entry_t *right)
{
    if (valid_identity_digest_equal(left, right)) {
        return true;
    }
    if (left && right &&
        (left->delivery_session_id != 0U ||
         right->delivery_session_id != 0U)) {
        return left->delivery_session_id != 0U &&
               left->delivery_session_id == right->delivery_session_id &&
               outbound_delivery_identity_material_equal(left, right);
    }
    if (!left || !right ||
        (left->identity_digest_valid && right->identity_digest_valid) ||
        left->seq != right->seq ||
        left->uptime_ms != right->uptime_ms ||
        left->ack_hash != right->ack_hash ||
        strncmp(left->contact_fingerprint, right->contact_fingerprint,
                sizeof(left->contact_fingerprint)) != 0 ||
        strncmp(left->direction, right->direction,
                sizeof(left->direction)) != 0 ||
        strncmp(left->text, right->text, sizeof(left->text)) != 0) {
        return false;
    }
    return true;
}

static uint8_t ack_state_merge_rank(d1l_dm_ack_state_t state)
{
    switch (state) {
    case D1L_DM_ACK_STATE_TERMINAL:
        return 4U;
    case D1L_DM_ACK_STATE_SENT:
        return 3U;
    case D1L_DM_ACK_STATE_RETRYABLE:
        return 2U;
    case D1L_DM_ACK_STATE_PENDING:
        return 1U;
    default:
        return 0U;
    }
}

static void copy_ack_metadata(d1l_dm_entry_t *dest,
                              const d1l_dm_entry_t *source)
{
    memcpy(dest->identity_digest, source->identity_digest,
           D1L_DM_IDENTITY_DIGEST_BYTES);
    dest->identity_digest_valid = source->identity_digest_valid;
    dest->ack_dispatch_count = source->ack_dispatch_count;
    dest->ack_dispatch_kind = source->ack_dispatch_kind;
    dest->ack_state = source->ack_state;
    dest->ack_last_error = source->ack_last_error;
}

static void merge_ack_metadata(d1l_dm_entry_t *dest,
                               const d1l_dm_entry_t *source,
                               bool source_newer)
{
    if (!dest || !source || !source->identity_digest_valid) {
        return;
    }
    if (!dest->identity_digest_valid) {
        copy_ack_metadata(dest, source);
        return;
    }
    if (memcmp(dest->identity_digest, source->identity_digest,
               D1L_DM_IDENTITY_DIGEST_BYTES) != 0 ||
        source->ack_dispatch_count < dest->ack_dispatch_count) {
        return;
    }
    if (source->ack_dispatch_count > dest->ack_dispatch_count ||
        ack_state_merge_rank(source->ack_state) >
            ack_state_merge_rank(dest->ack_state) ||
        (source_newer && source->ack_state == dest->ack_state)) {
        copy_ack_metadata(dest, source);
    }
}

static void copy_delivery_metadata(d1l_dm_entry_t *dest,
                                   const d1l_dm_entry_t *source)
{
    dest->delivery_state = source->delivery_state;
    dest->delivery_reason = source->delivery_reason;
    dest->delivery_last_error = source->delivery_last_error;
    dest->delivery_retry_count = source->delivery_retry_count;
    dest->delivery_revision = source->delivery_revision;
    dest->delivered = source->delivered;
    dest->acked = source->acked;
}

static void merge_delivery_metadata(d1l_dm_entry_t *dest,
                                    const d1l_dm_entry_t *source,
                                    bool source_newer)
{
    if (!dest || !source ||
        strncmp(dest->direction, "tx", sizeof(dest->direction)) != 0 ||
        strncmp(source->direction, "tx", sizeof(source->direction)) != 0) {
        return;
    }
    if (source->delivery_revision > dest->delivery_revision ||
        (source_newer &&
         source->delivery_revision == dest->delivery_revision)) {
        copy_delivery_metadata(dest, source);
    }
}

static void sort_entries_by_seq(d1l_dm_entry_t *entries, size_t count)
{
    for (size_t i = 1U; i < count; ++i) {
        const d1l_dm_entry_t ordered = entries[i];
        size_t position = i;
        while (position > 0U && entries[position - 1U].seq > ordered.seq) {
            entries[position] = entries[position - 1U];
            position--;
        }
        entries[position] = ordered;
    }
}

static uint32_t max_u32(uint32_t left, uint32_t right)
{
    return left > right ? left : right;
}

static esp_err_t append_resequenced_locked(const d1l_dm_entry_t *source)
{
    if (!source || s_next_seq == UINT32_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_dm_entry_t entry = *source;
    entry.seq = s_next_seq++;
    s_entries[s_head] = entry;
    s_head = (s_head + 1U) % D1L_DM_STORE_CAPACITY;
    if (s_count < D1L_DM_STORE_CAPACITY) {
        s_count++;
    }
    return ESP_OK;
}

static esp_err_t merge_same_epoch_locked(const d1l_dm_store_blob_t *primary,
                                         bool *out_changed)
{
    if (!primary || !out_changed) {
        return ESP_ERR_INVALID_ARG;
    }
    fill_blob_locked(&s_compare_blob_scratch);
    size_t merged_count = s_compare_blob_scratch.count;
    memcpy(s_merge_entries, s_compare_blob_scratch.entries,
           merged_count * sizeof(s_merge_entries[0]));
    uint32_t next_seq = max_u32(s_compare_blob_scratch.next_seq,
                                primary->next_seq);
    const bool primary_newer =
        primary->content_revision > s_compare_blob_scratch.content_revision;
    uint32_t additional_count = 0U;
    uint32_t retained_live_count = 0U;

    if (s_mutated_since_init) {
        for (size_t current = 0U; current < merged_count; ++current) {
            if (s_merge_entries[current].seq < s_boot_next_seq) {
                continue;
            }
            retained_live_count++;
            bool exact_on_sd = false;
            for (size_t source = 0U; source < primary->count; ++source) {
                if (entry_identity_equal(&s_merge_entries[current],
                                         &primary->entries[source])) {
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

    for (size_t source = 0U; source < primary->count; ++source) {
        d1l_dm_entry_t candidate = primary->entries[source];
        size_t identity_match = 0U;
        bool identity_match_set = false;
        for (size_t current = 0U; current < merged_count; ++current) {
            if (delivery_session_id_conflicts(&s_merge_entries[current],
                                              &candidate)) {
                return ESP_ERR_INVALID_STATE;
            }
            if (entry_identity_equal(&s_merge_entries[current], &candidate)) {
                identity_match = current;
                identity_match_set = true;
                break;
            }
        }
        if (identity_match_set) {
            const bool outbound = strncmp(
                s_merge_entries[identity_match].direction, "tx",
                sizeof(s_merge_entries[identity_match].direction)) == 0;
            if (!outbound) {
                s_merge_entries[identity_match].delivered =
                    s_merge_entries[identity_match].delivered ||
                    candidate.delivered;
                s_merge_entries[identity_match].acked =
                    s_merge_entries[identity_match].acked || candidate.acked;
            }
            if (candidate.attempt > s_merge_entries[identity_match].attempt) {
                s_merge_entries[identity_match].attempt = candidate.attempt;
            }
            merge_ack_metadata(&s_merge_entries[identity_match], &candidate,
                               primary_newer);
            merge_delivery_metadata(&s_merge_entries[identity_match],
                                    &candidate, primary_newer);
            continue;
        }
        size_t seq_match = 0U;
        bool seq_match_set = false;
        for (size_t current = 0U; current < merged_count; ++current) {
            if (s_merge_entries[current].seq == candidate.seq) {
                seq_match = current;
                seq_match_set = true;
                break;
            }
        }
        if (seq_match_set) {
            if (next_seq == UINT32_MAX) {
                return ESP_ERR_INVALID_STATE;
            }
            const bool current_is_live = s_mutated_since_init &&
                s_merge_entries[seq_match].seq >= s_boot_next_seq;
            if (current_is_live) {
                if (merged_count >= D1L_DM_STORE_CAPACITY * 2U) {
                    return ESP_ERR_INVALID_SIZE;
                }
                d1l_dm_entry_t live = s_merge_entries[seq_match];
                live.seq = next_seq++;
                s_merge_entries[seq_match] = candidate;
                s_merge_entries[merged_count++] = live;
                continue;
            }
            candidate.seq = next_seq++;
            additional_count++;
        }
        if (merged_count < D1L_DM_STORE_CAPACITY * 2U) {
            s_merge_entries[merged_count++] = candidate;
        }
    }

    sort_entries_by_seq(s_merge_entries, merged_count);
    const size_t keep_from = merged_count > D1L_DM_STORE_CAPACITY ?
        merged_count - D1L_DM_STORE_CAPACITY : 0U;
    const size_t keep_count = merged_count - keep_from;
    clear_history_locked();
    memcpy(s_entries, s_merge_entries + keep_from,
           keep_count * sizeof(s_entries[0]));
    s_count = keep_count;
    s_head = keep_count % D1L_DM_STORE_CAPACITY;
    s_epoch = primary->epoch;
    const uint32_t local_base_total = s_mutated_since_init ?
        (s_compare_blob_scratch.total_written >= s_mutations_since_init ?
         s_compare_blob_scratch.total_written - s_mutations_since_init : 0U) :
        s_compare_blob_scratch.total_written;
    s_total_written = max_u32(local_base_total, primary->total_written);
    if (additional_count > 0U) {
        s_total_written = s_total_written > UINT32_MAX - additional_count ?
            UINT32_MAX : s_total_written + additional_count;
    }
    if (s_total_written < keep_count) {
        s_total_written = (uint32_t)keep_count;
    }
    s_dropped_oldest = s_total_written >= s_count ?
        s_total_written - (uint32_t)s_count : 0U;
    s_next_seq = next_seq;
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[i].seq >= s_next_seq && s_entries[i].seq < UINT32_MAX) {
            s_next_seq = s_entries[i].seq + 1U;
        }
    }
    s_content_revision = max_u32(s_compare_blob_scratch.content_revision,
                                 primary->content_revision);
    fill_blob_locked(&s_nvs_blob_scratch);
    const bool changed = !blobs_equal(&s_compare_blob_scratch,
                                      &s_nvs_blob_scratch);
    if (changed && s_content_revision < UINT32_MAX) {
        s_content_revision++;
    }
    *out_changed = changed;
    return ESP_OK;
}

static esp_err_t merge_primary_locked(const d1l_dm_store_blob_t *primary,
                                      bool *out_changed)
{
    if (!primary || !out_changed) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_changed = false;
    fill_blob_locked(&s_compare_blob_scratch);
    const bool incoming_clear_dominates =
        !s_device_lineage_authoritative &&
        primary->clear_lineage != s_clear_lineage;
    if (primary->clear_lineage != s_clear_lineage &&
        !incoming_clear_dominates) {
        /* The nonzero device-local NVS lineage is authoritative. Random
         * tokens identify clear branches and are never numerically ordered. */
        return ESP_OK;
    }
    if (!incoming_clear_dominates) {
        if (primary->epoch < s_epoch) {
            return ESP_OK;
        }
        if (primary->epoch == s_epoch) {
            return merge_same_epoch_locked(primary, out_changed);
        }
    }

    size_t overlay_count = 0U;
    const uint32_t overlay_mutations = s_mutations_since_init;
    uint32_t overlay_total_additions = overlay_mutations;
    const uint32_t overlay_content_revision = s_content_revision;
    const size_t oldest = s_count == 0U ? 0U :
        (s_head + D1L_DM_STORE_CAPACITY - s_count) % D1L_DM_STORE_CAPACITY;
    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_dm_entry_t *entry =
            &s_entries[(oldest + i) % D1L_DM_STORE_CAPACITY];
        /* Preserve authenticated ACK metadata for identities present on both
         * backends even when a higher-epoch primary wins the message history.
         * Only genuinely live post-boot rows may be replayed when absent. */
        if (entry->identity_digest_valid ||
            (s_mutated_since_init && entry->seq >= s_boot_next_seq)) {
            s_overlay_entries[overlay_count++] = *entry;
        }
    }
    sort_entries_by_seq(s_overlay_entries, overlay_count);
    load_blob_locked(primary);
    bool result_differs_primary = false;
    for (size_t i = 0U; i < overlay_count; ++i) {
        d1l_dm_entry_t *identity_match = NULL;
        for (size_t current = 0U; current < s_count; ++current) {
            if (delivery_session_id_conflicts(&s_entries[current],
                                              &s_overlay_entries[i])) {
                load_blob_locked(&s_compare_blob_scratch);
                return ESP_ERR_INVALID_STATE;
            }
            if (entry_identity_equal(&s_entries[current],
                                     &s_overlay_entries[i])) {
                identity_match = &s_entries[current];
                break;
            }
        }
        if (identity_match) {
            const d1l_dm_entry_t before = *identity_match;
            const bool outbound = strncmp(
                identity_match->direction, "tx",
                sizeof(identity_match->direction)) == 0;
            if (!outbound) {
                identity_match->delivered = identity_match->delivered ||
                                            s_overlay_entries[i].delivered;
                identity_match->acked = identity_match->acked ||
                                        s_overlay_entries[i].acked;
            }
            if (s_overlay_entries[i].attempt > identity_match->attempt) {
                identity_match->attempt = s_overlay_entries[i].attempt;
            }
            merge_ack_metadata(identity_match, &s_overlay_entries[i], true);
            merge_delivery_metadata(identity_match, &s_overlay_entries[i],
                                    true);
            result_differs_primary = result_differs_primary ||
                memcmp(&before, identity_match, sizeof(before)) != 0;
            continue;
        }
        const bool replay_live = s_mutated_since_init &&
            s_overlay_entries[i].seq >= s_boot_next_seq;
        const bool preserve_pending =
            s_overlay_entries[i].identity_digest_valid &&
            s_overlay_entries[i].ack_state == D1L_DM_ACK_STATE_PENDING &&
            s_overlay_entries[i].ack_dispatch_count > 0U;
        if (!replay_live && !preserve_pending) {
            continue;
        }
        const esp_err_t replay_ret =
            append_resequenced_locked(&s_overlay_entries[i]);
        if (replay_ret != ESP_OK) {
            /* Restore the pre-merge snapshot; no target may be overwritten. */
            load_blob_locked(&s_compare_blob_scratch);
            return replay_ret;
        }
        if (preserve_pending && !replay_live &&
            overlay_total_additions < UINT32_MAX) {
            overlay_total_additions++;
        }
        result_differs_primary = true;
    }
    s_total_written =
        primary->total_written > UINT32_MAX - overlay_total_additions ?
            UINT32_MAX : primary->total_written + overlay_total_additions;
    s_dropped_oldest = s_total_written >= s_count ?
        s_total_written - (uint32_t)s_count : 0U;
    if (overlay_content_revision > s_content_revision) {
        s_content_revision = overlay_content_revision;
        result_differs_primary = true;
    }
    if (overlay_total_additions > 0U) {
        result_differs_primary = true;
    }
    if (result_differs_primary && s_content_revision < UINT32_MAX) {
        s_content_revision++;
    }
    *out_changed = !blobs_equal(&s_compare_blob_scratch, primary) ||
                   result_differs_primary;
    return ESP_OK;
}

static bool persistence_dirty_locked(bool sd_primary_required)
{
    return s_nvs_fallback_dirty ||
           (sd_primary_required &&
            (s_sd_primary_dirty || s_sd_reconcile_pending));
}

static bool backend_generation_matches(uint32_t expected_generation)
{
    d1l_retained_blob_store_backend_state_t state = {0};
    return d1l_retained_blob_store_backend_state(D1L_DM_STORE_ID, &state) &&
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
    d1l_store_lock_give(&s_store_lock);
    return failure;
}

static esp_err_t reconcile_sd_primary(uint32_t expected_generation)
{
    if (!backend_generation_matches(expected_generation)) {
        return note_sd_reconcile_failure(ESP_ERR_INVALID_STATE);
    }
    bool upgrade = false;
    const esp_err_t read_ret = read_decoded_blob(true, &s_sd_blob_scratch,
                                                &upgrade);
    if (read_ret == ESP_ERR_NOT_FINISHED) {
        return read_ret;
    }
    if (read_ret != ESP_OK && read_ret != ESP_ERR_NOT_FOUND) {
        return note_sd_reconcile_failure(read_ret);
    }
    if (!backend_generation_matches(expected_generation)) {
        return note_sd_reconcile_failure(ESP_ERR_INVALID_STATE);
    }

    d1l_store_lock_take(&s_store_lock);
    if (read_ret == ESP_ERR_NOT_FOUND) {
        s_sd_reconcile_pending = false;
        s_sd_primary_last_error = ESP_OK;
        s_sd_primary_dirty = s_count > 0U || s_epoch > 1U ||
                             s_clear_lineage != 0U;
        d1l_store_lock_give(&s_store_lock);
        return ESP_OK;
    }

    fill_blob_locked(&s_persist_snapshot.blob);
    s_reconcile_volatile_scratch = s_volatile_entry;
    const bool rollback_volatile_valid = s_volatile_valid;
    fill_blob_locked(&s_compare_blob_scratch);
    bool state_changed = false;
    const esp_err_t merge_ret = merge_primary_locked(&s_sd_blob_scratch,
                                                      &state_changed);
    /* Reconciliation is durable-state maintenance; it must not consume a UI
     * preview that deliberately lives outside the retained ring. */
    s_volatile_entry = s_reconcile_volatile_scratch;
    s_volatile_valid = rollback_volatile_valid;
    if (merge_ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        return note_sd_reconcile_failure(merge_ret);
    }
    if (!backend_generation_matches(expected_generation)) {
        load_blob_locked(&s_persist_snapshot.blob);
        s_volatile_entry = s_reconcile_volatile_scratch;
        s_volatile_valid = rollback_volatile_valid;
        d1l_store_lock_give(&s_store_lock);
        return note_sd_reconcile_failure(ESP_ERR_INVALID_STATE);
    }
    fill_blob_locked(&s_nvs_blob_scratch);
    s_sd_primary_dirty = upgrade ||
        !blobs_equal(&s_nvs_blob_scratch, &s_sd_blob_scratch);
    s_sd_reconcile_pending = false;
    s_sd_primary_last_error = ESP_OK;
    if (state_changed) {
        s_state_revision++;
        s_nvs_fallback_dirty = true;
    }
    if (!s_device_lineage_authoritative) {
        /* Accept a removable lineage only when no valid NVS state or local
         * mutation established device authority first. */
        s_device_lineage_authoritative = true;
    }
    if (!s_mutated_since_init) {
        s_boot_next_seq = s_next_seq;
    }
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

static void update_nvs_result(esp_err_t nvs_ret, uint64_t snapshot_revision,
                              bool sd_primary_required)
{
    d1l_store_lock_take(&s_store_lock);
    const bool same_revision = s_state_revision == snapshot_revision;
    s_nvs_fallback_last_error = nvs_ret;
    if (nvs_ret == ESP_OK) {
        s_nvs_fallback_commit_count++;
        if (same_revision) {
            s_nvs_fallback_dirty = false;
        }
    } else {
        s_nvs_fallback_fail_count++;
        s_persistence_fail_count++;
        s_retry_pending = true;
    }
    if (!same_revision) {
        s_persistence_stale_snapshot_count++;
    }
    if (!persistence_dirty_locked(sd_primary_required)) {
        s_retry_pending = false;
        s_persistence_commit_count++;
        if (!s_sd_reconcile_pending &&
            (!sd_primary_required || !s_sd_primary_dirty)) {
            s_mutated_since_init = false;
            s_mutations_since_init = 0U;
            s_boot_next_seq = s_next_seq;
        }
    }
    d1l_store_lock_give(&s_store_lock);
}

static esp_err_t persist_nvs_after_reconcile_failure(bool sd_primary_required)
{
    d1l_store_lock_take(&s_store_lock);
    if (!s_nvs_fallback_dirty) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_OK;
    }
    fill_blob_locked(&s_persist_snapshot.blob);
    s_persist_snapshot.state_revision = s_state_revision;
    d1l_store_lock_give(&s_store_lock);

    const esp_err_t ret = d1l_retained_blob_store_write_nvs_fallback(
        D1L_DM_STORE_ID, D1L_DM_STORE_KEY,
        &s_persist_snapshot.blob, sizeof(s_persist_snapshot.blob));
    update_nvs_result(ret, s_persist_snapshot.state_revision,
                      sd_primary_required);
    return ret;
}

static esp_err_t persist_store(bool force)
{
    d1l_store_lock_take(&s_persist_io_lock);
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    if (!d1l_retained_blob_store_backend_state(D1L_DM_STORE_ID,
                                               &backend_state)) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool sd_primary_required = backend_state.enabled;
    const uint32_t attempt_ms = now_ms();

    d1l_store_lock_take(&s_store_lock);
    if (!s_loaded) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (backend_state.generation != s_last_sd_backend_generation) {
        s_last_sd_backend_generation = backend_state.generation;
        s_sd_reconcile_pending = true;
        s_retry_pending = false;
    }
    s_last_sd_primary_required = sd_primary_required;
    if (!persistence_dirty_locked(sd_primary_required)) {
        acknowledge_persisted_delivery_truth_locked();
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }
    const bool retry_ready = !s_persist_attempted_this_boot ||
        attempt_ms - s_last_persist_attempt_ms >=
            D1L_DM_STORE_PERSIST_RETRY_INTERVAL_MS;
    if (s_retry_pending && !retry_ready) {
        esp_err_t pending = s_sd_primary_last_error != ESP_OK ?
            s_sd_primary_last_error : s_nvs_fallback_last_error;
        if (pending == ESP_OK) {
            pending = ESP_ERR_INVALID_STATE;
        }
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return force ? pending : ESP_OK;
    }
    const bool reconcile_attempt =
        sd_primary_required && s_sd_reconcile_pending;
    s_persist_attempted_this_boot = true;
    s_last_persist_attempt_ms = attempt_ms;
    d1l_store_lock_give(&s_store_lock);

    if (reconcile_attempt) {
        const esp_err_t reconcile_ret = reconcile_sd_primary(
            backend_state.generation);
        if (reconcile_ret != ESP_OK) {
            if (reconcile_ret == ESP_ERR_NOT_FINISHED) {
                d1l_store_lock_give(&s_persist_io_lock);
                return reconcile_ret;
            }
            (void)persist_nvs_after_reconcile_failure(sd_primary_required);
            d1l_store_lock_give(&s_persist_io_lock);
            return reconcile_ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    fill_blob_locked(&s_persist_snapshot.blob);
    s_persist_snapshot.state_revision = s_state_revision;
    s_persist_snapshot.sd_attempt =
        sd_primary_required && s_sd_primary_dirty;
    s_persist_snapshot.nvs_attempt = s_nvs_fallback_dirty;
    d1l_store_lock_give(&s_store_lock);

    esp_err_t sd_ret = ESP_OK;
    esp_err_t nvs_ret = ESP_OK;
    if (s_persist_snapshot.sd_attempt) {
        if (!backend_generation_matches(backend_state.generation)) {
            sd_ret = ESP_ERR_INVALID_STATE;
        } else {
            sd_ret = d1l_retained_blob_store_write_sd_primary_guarded(
                D1L_DM_STORE_ID, D1L_DM_STORE_KEY,
                &s_persist_snapshot.blob, sizeof(s_persist_snapshot.blob),
                backend_state.generation);
            if (sd_ret == ESP_ERR_NOT_FINISHED) {
                d1l_store_lock_give(&s_persist_io_lock);
                return sd_ret;
            }
            if (!backend_generation_matches(backend_state.generation)) {
                sd_ret = ESP_ERR_INVALID_STATE;
            }
        }
    }
    if (s_persist_snapshot.nvs_attempt) {
        nvs_ret = d1l_retained_blob_store_write_nvs_fallback(
            D1L_DM_STORE_ID, D1L_DM_STORE_KEY,
            &s_persist_snapshot.blob, sizeof(s_persist_snapshot.blob));
    }

    d1l_store_lock_take(&s_store_lock);
    const bool same_revision =
        s_state_revision == s_persist_snapshot.state_revision;
    if (s_persist_snapshot.sd_attempt) {
        s_sd_primary_last_error = sd_ret;
        if (sd_ret == ESP_OK) {
            s_sd_primary_commit_count++;
            if (same_revision) {
                s_sd_primary_dirty = false;
            }
        } else {
            s_sd_primary_fail_count++;
            if (sd_ret == ESP_ERR_INVALID_STATE) {
                s_sd_reconcile_pending = true;
            }
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
    const bool still_dirty = persistence_dirty_locked(sd_primary_required);
    if (still_dirty && (operation_failed || !same_revision)) {
        s_retry_pending = true;
    }
    if (!still_dirty) {
        acknowledge_persisted_delivery_truth_locked();
        s_retry_pending = false;
        s_persistence_commit_count++;
        if (!s_sd_reconcile_pending &&
            (!sd_primary_required || !s_sd_primary_dirty)) {
            s_mutated_since_init = false;
            s_mutations_since_init = 0U;
            s_boot_next_seq = s_next_seq;
        }
    }
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
    const esp_err_t ret = d1l_dm_store_init();
    d1l_store_lock_take(&s_store_lock);
    const bool fallback_loaded = s_loaded;
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_init_recovery_lock);
    return fallback_loaded ? ESP_OK : ret;
}

esp_err_t d1l_dm_store_init(void)
{
    d1l_store_lock_take(&s_persist_io_lock);
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    if (!d1l_retained_blob_store_backend_state(D1L_DM_STORE_ID,
                                               &backend_state)) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }

    bool nvs_upgrade = false;
    bool sd_upgrade = false;
    const esp_err_t nvs_ret = read_decoded_blob(false, &s_nvs_blob_scratch,
                                               &nvs_upgrade);
    esp_err_t sd_ret = ESP_ERR_NOT_FOUND;
    if (backend_state.enabled) {
        sd_ret = read_decoded_blob(true, &s_sd_blob_scratch, &sd_upgrade);
        if (!backend_generation_matches(backend_state.generation)) {
            sd_ret = ESP_ERR_INVALID_STATE;
        }
    }
    const bool nvs_valid = nvs_ret == ESP_OK;
    const bool sd_valid = sd_ret == ESP_OK;
    bool sd_accepted = sd_valid;
    const bool sd_problem = backend_state.enabled &&
        sd_ret != ESP_OK && sd_ret != ESP_ERR_NOT_FOUND;
    const bool nvs_problem =
        nvs_ret != ESP_OK && nvs_ret != ESP_ERR_NOT_FOUND;
    esp_err_t init_merge_ret = ESP_OK;

    d1l_store_lock_take(&s_store_lock);
    reset_store_locked(backend_state.enabled, backend_state.generation);
    if (nvs_valid) {
        load_blob_locked(&s_nvs_blob_scratch);
    } else if (sd_valid) {
        load_blob_locked(&s_sd_blob_scratch);
    }
    s_device_lineage_authoritative = nvs_valid || sd_valid;
    s_boot_next_seq = s_next_seq;
    s_mutated_since_init = false;

    if (nvs_valid && sd_valid) {
        bool state_changed = false;
        init_merge_ret = merge_primary_locked(&s_sd_blob_scratch,
                                               &state_changed);
        if (init_merge_ret != ESP_OK) {
            load_blob_locked(&s_nvs_blob_scratch);
        } else if (state_changed) {
            s_state_revision++;
        }
    }
    bool init_generation_changed = false;
    if (backend_state.enabled &&
        !backend_generation_matches(backend_state.generation)) {
        init_generation_changed = true;
        sd_accepted = false;
        sd_ret = ESP_ERR_INVALID_STATE;
        if (nvs_valid) {
            load_blob_locked(&s_nvs_blob_scratch);
            s_device_lineage_authoritative = true;
        } else {
            reset_store_locked(backend_state.enabled,
                               backend_state.generation);
        }
    }
    (void)normalize_interrupted_ack_locked();
    (void)normalize_interrupted_delivery_locked();
    const bool effective_sd_problem = sd_problem ||
        init_merge_ret != ESP_OK || init_generation_changed;
    fill_blob_locked(&s_compare_blob_scratch);
    s_nvs_fallback_dirty = nvs_upgrade || !nvs_valid ||
        (nvs_valid && !blobs_equal(&s_compare_blob_scratch,
                                   &s_nvs_blob_scratch));
    if (!nvs_valid && !sd_accepted && !nvs_problem) {
        s_nvs_fallback_dirty = false;
    }
    if (backend_state.enabled) {
        s_sd_reconcile_pending = effective_sd_problem;
        s_sd_primary_dirty = !effective_sd_problem &&
            (sd_upgrade || !sd_accepted ||
             (sd_accepted && !blobs_equal(&s_compare_blob_scratch,
                                       &s_sd_blob_scratch)));
        if (!sd_accepted && sd_ret == ESP_ERR_NOT_FOUND &&
            s_count == 0U && s_epoch == 1U && s_clear_lineage == 0U) {
            s_sd_primary_dirty = false;
        }
    } else {
        s_sd_reconcile_pending = true;
        s_sd_primary_dirty = false;
    }
    if (effective_sd_problem) {
        s_sd_primary_last_error = init_merge_ret != ESP_OK ?
            init_merge_ret : sd_ret;
        s_sd_primary_fail_count++;
        s_persistence_fail_count++;
        s_retry_pending = true;
    }
    if (nvs_problem) {
        s_nvs_fallback_last_error = nvs_ret;
        s_nvs_fallback_fail_count++;
        s_persistence_fail_count++;
        s_nvs_fallback_dirty = true;
        s_retry_pending = true;
    }
    if (!s_mutated_since_init) {
        s_boot_next_seq = s_next_seq;
    }
    const bool no_valid_source_with_error =
        !nvs_valid && !sd_accepted &&
        (nvs_problem || effective_sd_problem);
    s_loaded = !no_valid_source_with_error;
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);

    if (!nvs_valid && !sd_accepted) {
        if (effective_sd_problem) {
            return sd_ret;
        }
        if (nvs_problem) {
            return nvs_ret;
        }
    }
    return ESP_OK;
}

esp_err_t d1l_dm_store_clear(void)
{
    esp_err_t ret = ensure_store_initialized();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_store_lock_take(&s_store_lock);
    const uint64_t lineage = new_clear_lineage();
    clear_history_locked();
    s_epoch = s_epoch == UINT32_MAX ? 1U : s_epoch + 1U;
    s_clear_lineage = lineage;
    s_content_revision = s_content_revision == UINT32_MAX ?
        UINT32_MAX : s_content_revision + 1U;
    s_state_revision++;
    s_mutated_since_init = true;
    s_device_lineage_authoritative = true;
    s_boot_next_seq = s_next_seq;
    s_mutations_since_init = 0U;
    s_sd_primary_dirty = true;
    s_nvs_fallback_dirty = true;
    d1l_store_lock_give(&s_store_lock);
    return d1l_dm_store_flush();
}

esp_err_t d1l_dm_store_flush(void)
{
    esp_err_t ret = ensure_store_initialized();
    return ret == ESP_OK ? persist_store(true) : ret;
}

esp_err_t d1l_dm_store_flush_if_due(void)
{
    esp_err_t ret = ensure_store_initialized();
    return ret == ESP_OK ? persist_store(false) : ret;
}

static esp_err_t append_internal(const char *contact_fingerprint,
                                 const char *contact_alias,
                                 const char *direction, const char *text,
                                 int rssi_dbm, int snr_tenths,
                                  uint8_t path_hash_bytes, uint8_t path_hops,
                                  uint8_t attempt, bool delivered, bool acked,
                                  uint32_t ack_hash, bool persist,
                                  const uint8_t *identity_digest,
                                  d1l_dm_store_append_outcome_t *outcome)
{
    if (outcome) {
        memset(outcome, 0, sizeof(*outcome));
        outcome->error = ESP_ERR_INVALID_ARG;
    }
    const d1l_user_text_info_t text_info = d1l_user_text_validate(text);
    if (!contact_fingerprint || contact_fingerprint[0] == '\0' ||
        (text_info.result != D1L_USER_TEXT_OK &&
         text_info.result != D1L_USER_TEXT_TOO_LONG) ||
        path_hash_bytes > 3U ||
        path_hops > 63U ||
        (uint16_t)path_hash_bytes * path_hops > 64U ||
        (identity_digest && (!persist || !direction ||
                             strncmp(direction, "rx", D1L_DM_DIRECTION_LEN) != 0))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (text_info.result == D1L_USER_TEXT_TOO_LONG) {
        if (outcome) {
            outcome->error = ESP_ERR_INVALID_SIZE;
        }
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = ensure_store_initialized();
    if (ret != ESP_OK) {
        if (outcome) {
            outcome->error = ret;
        }
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    d1l_dm_entry_t entry = {
        .seq = s_next_seq,
        .uptime_ms = now_ms(),
        .rssi_dbm = rssi_dbm,
        .snr_tenths = snr_tenths,
        .path_hash_bytes = path_hash_bytes,
        .path_hops = path_hops,
        .attempt = attempt,
        .delivered = delivered,
        .acked = acked,
        .ack_hash = ack_hash,
        .ack_state = identity_digest ? D1L_DM_ACK_STATE_RETRYABLE :
                                       D1L_DM_ACK_STATE_LEGACY_UNVERIFIED,
        .ack_last_error = ESP_OK,
        .identity_digest_valid = identity_digest != NULL,
    };
    if (identity_digest) {
        memcpy(entry.identity_digest, identity_digest,
               D1L_DM_IDENTITY_DIGEST_BYTES);
    }
    sanitize_ascii(entry.contact_fingerprint,
                   sizeof(entry.contact_fingerprint), contact_fingerprint);
    sanitize_ascii(entry.contact_alias, sizeof(entry.contact_alias),
                   contact_alias && contact_alias[0] ?
                       contact_alias : contact_fingerprint);
    sanitize_ascii(entry.direction, sizeof(entry.direction),
                   direction && direction[0] ? direction : "rx");
    memcpy(entry.text, text, text_info.byte_count + 1U);
    if (strncmp(entry.direction, "tx", sizeof(entry.direction)) == 0) {
        entry.delivery_state = acked ? D1L_DM_DELIVERY_ACKNOWLEDGED :
                                       D1L_DM_DELIVERY_QUEUED;
        entry.delivery_reason = acked ? D1L_DM_DELIVERY_REASON_ACK_RECEIVED :
                                        D1L_DM_DELIVERY_REASON_ENQUEUED;
        entry.delivery_last_error = ESP_OK;
        entry.delivery_retry_count = 0U;
        entry.delivery_revision = 1U;
        entry.acked = acked;
        entry.delivered = acked;
    } else {
        entry.delivery_state = D1L_DM_DELIVERY_NOT_APPLICABLE;
        entry.delivery_reason = D1L_DM_DELIVERY_REASON_NONE;
        entry.delivery_last_error = ESP_OK;
        entry.delivery_retry_count = 0U;
        entry.delivery_revision = 0U;
    }

    if (!persist) {
        s_volatile_entry = entry;
        s_volatile_valid = true;
        d1l_store_lock_give(&s_store_lock);
        if (outcome) {
            outcome->error = ESP_OK;
        }
        return ESP_OK;
    }

    if (s_count == D1L_DM_STORE_CAPACITY) {
        const d1l_dm_entry_t *oldest = &s_entries[s_head];
        const bool oldest_is_tx =
            strncmp(oldest->direction, "tx", sizeof(oldest->direction)) == 0;
        d1l_dm_delivery_state_t oldest_delivery_state =
            oldest->delivery_state;
        if (oldest_is_tx) {
            const d1l_dm_ack_persistence_pending_t *pending =
                find_ack_persistence_pending_locked(
                    oldest->delivery_session_id,
                    oldest->delivery_revision);
            if (pending) {
                oldest_delivery_state = pending->previous_state;
            }
        }
        if (oldest_is_tx &&
            !d1l_dm_delivery_state_terminal(oldest_delivery_state)) {
            /* Preserve ring order and the stable delivery-session owner.  A
             * later append may evict this row only after its exact transition
             * makes the oldest TX terminal. */
            d1l_store_lock_give(&s_store_lock);
            if (outcome) {
                outcome->error = ESP_ERR_NO_MEM;
            }
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_next_seq == UINT32_MAX || s_total_written == UINT32_MAX ||
        (s_count == D1L_DM_STORE_CAPACITY &&
         s_dropped_oldest == UINT32_MAX)) {
        d1l_store_lock_give(&s_store_lock);
        if (outcome) {
            outcome->error = ESP_ERR_INVALID_STATE;
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (strncmp(entry.direction, "tx", sizeof(entry.direction)) == 0) {
        entry.delivery_session_id = new_delivery_session_id_locked();
        if (entry.delivery_session_id == 0U) {
            d1l_store_lock_give(&s_store_lock);
            if (outcome) {
                outcome->error = ESP_ERR_INVALID_STATE;
            }
            return ESP_ERR_INVALID_STATE;
        }
    }
    s_volatile_valid = false;
    s_next_seq++;
    s_entries[s_head] = entry;
    s_head = (s_head + 1U) % D1L_DM_STORE_CAPACITY;
    if (s_count < D1L_DM_STORE_CAPACITY) {
        s_count++;
    } else {
        s_dropped_oldest++;
    }
    s_total_written++;
    if (s_content_revision < UINT32_MAX) {
        s_content_revision++;
    }
    s_state_revision++;
    s_mutated_since_init = true;
    s_device_lineage_authoritative = true;
    if (s_mutations_since_init < UINT32_MAX) {
        s_mutations_since_init++;
    }
    s_sd_primary_dirty = true;
    s_nvs_fallback_dirty = true;
    d1l_store_lock_give(&s_store_lock);
    if (outcome) {
        outcome->inserted = true;
        outcome->row_seq = entry.seq;
        outcome->delivery_session_id = entry.delivery_session_id;
    }
    ret = d1l_dm_store_flush();
    if (outcome) {
        outcome->durable = ret == ESP_OK;
        outcome->error = ret;
    }
    return ret;
}

esp_err_t d1l_dm_store_append(const char *contact_fingerprint,
                              const char *contact_alias,
                              const char *direction, const char *text,
                              int rssi_dbm, int snr_tenths,
                              uint8_t path_hash_bytes, uint8_t path_hops,
                              uint8_t attempt, bool delivered, bool acked,
                              uint32_t ack_hash)
{
    return append_internal(contact_fingerprint, contact_alias, direction, text,
                           rssi_dbm, snr_tenths, path_hash_bytes, path_hops,
                           attempt, delivered, acked, ack_hash, true, NULL,
                           NULL);
}

esp_err_t d1l_dm_store_append_tx(
    const char *contact_fingerprint, const char *contact_alias,
    const char *text, int rssi_dbm, int snr_tenths,
    uint8_t path_hash_bytes, uint8_t path_hops, uint8_t attempt,
    uint32_t ack_hash, d1l_dm_store_append_outcome_t *outcome)
{
    if (!outcome) {
        return ESP_ERR_INVALID_ARG;
    }
    return append_internal(contact_fingerprint, contact_alias, "tx", text,
                           rssi_dbm, snr_tenths, path_hash_bytes, path_hops,
                           attempt, false, false, ack_hash, true, NULL,
                           outcome);
}

esp_err_t d1l_dm_store_append_rx_identity(
    const char *contact_fingerprint, const char *contact_alias,
    const char *text, int rssi_dbm, int snr_tenths,
    uint8_t path_hash_bytes, uint8_t path_hops, uint8_t attempt,
    uint32_t ack_hash,
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    d1l_dm_store_append_outcome_t *outcome)
{
    if (!identity_digest || !outcome) {
        if (outcome) {
            memset(outcome, 0, sizeof(*outcome));
            outcome->error = ESP_ERR_INVALID_ARG;
        }
        return ESP_ERR_INVALID_ARG;
    }
    return append_internal(contact_fingerprint, contact_alias, "rx", text,
                           rssi_dbm, snr_tenths, path_hash_bytes, path_hops,
                           attempt, true, false, ack_hash, true,
                           identity_digest, outcome);
}

esp_err_t d1l_dm_store_append_volatile(const char *contact_fingerprint,
                                       const char *contact_alias,
                                       const char *direction, const char *text,
                                       int rssi_dbm, int snr_tenths,
                                       uint8_t path_hash_bytes,
                                       uint8_t path_hops, uint8_t attempt,
                                       bool delivered, bool acked,
                                       uint32_t ack_hash)
{
    return append_internal(contact_fingerprint, contact_alias, direction, text,
                           rssi_dbm, snr_tenths, path_hash_bytes, path_hops,
                           attempt, delivered, acked, ack_hash, false, NULL,
                           NULL);
}

static d1l_dm_entry_t *find_rx_identity_locked(
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    uint32_t row_seq)
{
    if (!identity_digest) {
        return NULL;
    }
    const size_t oldest = s_count == 0U ? 0U :
        (s_head + D1L_DM_STORE_CAPACITY - s_count) % D1L_DM_STORE_CAPACITY;
    for (size_t i = 0U; i < s_count; ++i) {
        d1l_dm_entry_t *entry =
            &s_entries[(oldest + i) % D1L_DM_STORE_CAPACITY];
        if (entry->identity_digest_valid &&
            (row_seq == 0U || entry->seq == row_seq) &&
            memcmp(entry->identity_digest, identity_digest,
                   D1L_DM_IDENTITY_DIGEST_BYTES) == 0) {
            return entry;
        }
    }
    return NULL;
}

bool d1l_dm_store_find_rx_identity(
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    d1l_dm_entry_t *out_entry)
{
    if (!identity_digest || ensure_store_initialized() != ESP_OK) {
        return false;
    }
    d1l_store_lock_take(&s_store_lock);
    const d1l_dm_entry_t *entry =
        find_rx_identity_locked(identity_digest, 0U);
    if (entry && out_entry) {
        *out_entry = *entry;
        project_ack_persistence_truth_locked(out_entry);
    }
    d1l_store_lock_give(&s_store_lock);
    return entry != NULL;
}

bool d1l_dm_store_find_delivery_session(
    uint64_t delivery_session_id, d1l_dm_entry_t *out_entry)
{
    if (out_entry) {
        memset(out_entry, 0, sizeof(*out_entry));
    }
    if (delivery_session_id == 0U || !out_entry ||
        ensure_store_initialized() != ESP_OK) {
        return false;
    }
    bool found = false;
    d1l_store_lock_take(&s_store_lock);
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[i].delivery_session_id != delivery_session_id) {
            continue;
        }
        *out_entry = s_entries[i];
        project_ack_persistence_truth_locked(out_entry);
        found = true;
        break;
    }
    d1l_store_lock_give(&s_store_lock);
    return found;
}

static void note_ack_metadata_mutation_locked(void)
{
    if (s_content_revision < UINT32_MAX) {
        s_content_revision++;
    }
    s_state_revision++;
    s_mutated_since_init = true;
    s_device_lineage_authoritative = true;
    s_sd_primary_dirty = true;
    s_nvs_fallback_dirty = true;
}

esp_err_t d1l_dm_store_reserve_ack_dispatch(
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    uint8_t dispatch_kind, d1l_dm_ack_reservation_t *reservation)
{
    if (reservation) {
        memset(reservation, 0, sizeof(*reservation));
        reservation->error = ESP_ERR_INVALID_ARG;
    }
    if (!identity_digest || !reservation || dispatch_kind == 0U ||
        dispatch_kind > D1L_DM_ACK_DISPATCH_KIND_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_store_initialized();
    if (ret != ESP_OK) {
        reservation->error = ret;
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    d1l_dm_entry_t *entry = find_rx_identity_locked(identity_digest, 0U);
    if (!entry || persistence_dirty_locked(s_last_sd_primary_required) ||
        (entry->ack_state != D1L_DM_ACK_STATE_RETRYABLE &&
         entry->ack_state != D1L_DM_ACK_STATE_SENT) ||
        entry->ack_dispatch_count >= D1L_DM_ACK_DISPATCH_MAX) {
        const esp_err_t failure = entry ? ESP_ERR_INVALID_STATE :
                                          ESP_ERR_NOT_FOUND;
        d1l_store_lock_give(&s_store_lock);
        reservation->error = failure;
        return reservation->error;
    }

    const uint32_t row_seq = entry->seq;
    const uint8_t previous_count = entry->ack_dispatch_count;
    entry->ack_dispatch_count++;
    entry->ack_dispatch_kind = dispatch_kind;
    entry->ack_state = D1L_DM_ACK_STATE_PENDING;
    entry->ack_last_error = ESP_OK;
    note_ack_metadata_mutation_locked();
    d1l_store_lock_give(&s_store_lock);

    ret = d1l_dm_store_flush();
    if (ret != ESP_OK) {
        /* A split SD/NVS write can partially commit before the other backend
         * reports failure. Keep the reservation dirty and pending; rolling it
         * back could regress a durable count and permit an ACK storm. */
        reservation->reserved = true;
        reservation->row_seq = row_seq;
        reservation->dispatch_count = (uint8_t)(previous_count + 1U);
        reservation->error = ret;
        return ret;
    }

    reservation->reserved = true;
    reservation->durable = true;
    reservation->row_seq = row_seq;
    reservation->dispatch_count = (uint8_t)(previous_count + 1U);
    reservation->error = ESP_OK;
    return ESP_OK;
}

esp_err_t d1l_dm_store_rebind_pending_ack_dispatch(
    uint32_t row_seq,
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    uint8_t dispatch_kind)
{
    if (row_seq == 0U || !identity_digest || dispatch_kind == 0U ||
        dispatch_kind > D1L_DM_ACK_DISPATCH_KIND_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_store_initialized();
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    d1l_dm_entry_t *entry =
        find_rx_identity_locked(identity_digest, row_seq);
    if (!entry || entry->ack_state != D1L_DM_ACK_STATE_PENDING ||
        entry->ack_dispatch_count == 0U) {
        const esp_err_t failure = entry ? ESP_ERR_INVALID_STATE :
                                          ESP_ERR_NOT_FOUND;
        d1l_store_lock_give(&s_store_lock);
        return failure;
    }
    if (entry->ack_dispatch_kind == dispatch_kind) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_OK;
    }
    entry->ack_dispatch_kind = dispatch_kind;
    note_ack_metadata_mutation_locked();
    d1l_store_lock_give(&s_store_lock);

    /* As with reservation, never roll this back after persistence starts: one
     * backend may already contain the rebound kind. Dirty pending state blocks
     * RF until a later flush makes that same reservation coherent. */
    return d1l_dm_store_flush();
}

esp_err_t d1l_dm_store_complete_ack_dispatch(
    uint32_t row_seq,
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    bool sent, esp_err_t error)
{
    if (row_seq == 0U || !identity_digest ||
        (sent && error != ESP_OK) || (!sent && error == ESP_OK)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_store_initialized();
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    /* Reconciliation may re-sequence a retained row while an already queued
     * RF response is in flight. The validated full digest is unique in v5 and
     * remains the primary callback correlation; row_seq is only a stale-safe
     * diagnostic hint after dispatch. */
    d1l_dm_entry_t *entry = find_rx_identity_locked(identity_digest, 0U);
    if (!entry || entry->ack_state != D1L_DM_ACK_STATE_PENDING ||
        entry->ack_dispatch_count == 0U) {
        const esp_err_t failure = entry ? ESP_ERR_INVALID_STATE :
                                          ESP_ERR_NOT_FOUND;
        d1l_store_lock_give(&s_store_lock);
        return failure;
    }
    if (sent) {
        entry->ack_state = D1L_DM_ACK_STATE_SENT;
        entry->ack_last_error = ESP_OK;
    } else {
        entry->ack_state =
            entry->ack_dispatch_count >= D1L_DM_ACK_DISPATCH_MAX ?
                D1L_DM_ACK_STATE_TERMINAL : D1L_DM_ACK_STATE_RETRYABLE;
        entry->ack_last_error = error;
    }
    note_ack_metadata_mutation_locked();
    d1l_store_lock_give(&s_store_lock);
    return d1l_dm_store_flush();
}

const char *d1l_dm_ack_state_name(d1l_dm_ack_state_t state)
{
    switch (state) {
    case D1L_DM_ACK_STATE_PENDING:
        return "pending";
    case D1L_DM_ACK_STATE_SENT:
        return "sent";
    case D1L_DM_ACK_STATE_RETRYABLE:
        return "retryable";
    case D1L_DM_ACK_STATE_TERMINAL:
        return "terminal";
    default:
        return "legacy_unverified";
    }
}

const char *d1l_dm_ack_dispatch_kind_name(uint8_t dispatch_kind)
{
    switch (dispatch_kind) {
    case 1U:
        return "flood_ack";
    case 2U:
        return "direct_ack";
    case 3U:
        return "flood_ack_path";
    default:
        return "none";
    }
}

static bool delivery_reason_matches_target(
    d1l_dm_delivery_state_t state, d1l_dm_delivery_reason_t reason)
{
    switch (state) {
    case D1L_DM_DELIVERY_QUEUED:
        return reason == D1L_DM_DELIVERY_REASON_ENQUEUED ||
               reason == D1L_DM_DELIVERY_REASON_USER_RETRY;
    case D1L_DM_DELIVERY_WAITING_RADIO:
        return reason == D1L_DM_DELIVERY_REASON_RADIO_RESERVED;
    case D1L_DM_DELIVERY_TX_ACTIVE:
        return reason == D1L_DM_DELIVERY_REASON_RADIO_STARTED;
    case D1L_DM_DELIVERY_TX_DONE:
        return reason == D1L_DM_DELIVERY_REASON_RADIO_COMPLETED;
    case D1L_DM_DELIVERY_AWAITING_ACK:
        return reason == D1L_DM_DELIVERY_REASON_ACK_EXPECTED;
    case D1L_DM_DELIVERY_ACKNOWLEDGED:
        return reason == D1L_DM_DELIVERY_REASON_ACK_RECEIVED ||
               reason == D1L_DM_DELIVERY_REASON_LEGACY_IMPORT;
    case D1L_DM_DELIVERY_RETRY_WAIT:
        return reason == D1L_DM_DELIVERY_REASON_RETRY_SCHEDULED;
    case D1L_DM_DELIVERY_RETRY_TX:
        return reason == D1L_DM_DELIVERY_REASON_RETRY_STARTED;
    case D1L_DM_DELIVERY_FAILED_RADIO:
        return reason == D1L_DM_DELIVERY_REASON_RADIO_ERROR;
    case D1L_DM_DELIVERY_FAILED_TIMEOUT:
        return reason == D1L_DM_DELIVERY_REASON_ACK_TIMEOUT;
    case D1L_DM_DELIVERY_FAILED_QUEUE:
        return reason == D1L_DM_DELIVERY_REASON_QUEUE_REJECTED;
    case D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT:
        return reason == D1L_DM_DELIVERY_REASON_REBOOT_RECOVERY ||
               reason == D1L_DM_DELIVERY_REASON_LEGACY_IMPORT;
    case D1L_DM_DELIVERY_CANCELLED:
        return reason == D1L_DM_DELIVERY_REASON_USER_CANCELLED;
    default:
        return false;
    }
}

static esp_err_t apply_delivery_transition_locked(
    d1l_dm_entry_t *entry, d1l_dm_delivery_state_t expected_state,
    uint32_t expected_delivery_revision,
    d1l_dm_delivery_state_t next_state,
    d1l_dm_delivery_reason_t reason, esp_err_t error)
{
    const bool advances_attempt =
        next_state == D1L_DM_DELIVERY_RETRY_TX ||
        (next_state == D1L_DM_DELIVERY_QUEUED &&
         reason == D1L_DM_DELIVERY_REASON_USER_RETRY);
    if (!entry ||
        strncmp(entry->direction, "tx", sizeof(entry->direction)) != 0 ||
        entry->delivery_state != expected_state ||
        entry->delivery_revision != expected_delivery_revision ||
        reason == D1L_DM_DELIVERY_REASON_LEGACY_IMPORT ||
        (next_state == D1L_DM_DELIVERY_QUEUED &&
         reason != D1L_DM_DELIVERY_REASON_USER_RETRY) ||
        !d1l_dm_delivery_transition_allowed(expected_state, next_state) ||
        !delivery_reason_matches_target(next_state, reason) ||
        (delivery_state_is_failure(next_state) && error == ESP_OK) ||
        (next_state == D1L_DM_DELIVERY_ACKNOWLEDGED && error != ESP_OK) ||
        entry->delivery_revision == UINT32_MAX ||
        (advances_attempt &&
         (entry->delivery_retry_count == UINT8_MAX ||
          entry->attempt == UINT8_MAX))) {
        return ESP_ERR_INVALID_STATE;
    }

    entry->delivery_state = next_state;
    entry->delivery_reason = reason;
    entry->delivery_last_error = error;
    entry->delivery_revision++;
    if (advances_attempt) {
        entry->delivery_retry_count++;
        entry->attempt++;
    }
    entry->acked = next_state == D1L_DM_DELIVERY_ACKNOWLEDGED;
    entry->delivered = entry->acked;
    return ESP_OK;
}

static esp_err_t transition_delivery_internal(
    uint64_t delivery_session_id,
    d1l_dm_delivery_state_t expected_state,
    uint32_t expected_delivery_revision,
    d1l_dm_delivery_state_t next_state,
    d1l_dm_delivery_reason_t reason, esp_err_t error,
    bool rebind_retry_ack, uint32_t retry_ack_hash,
    d1l_dm_delivery_transition_outcome_t *outcome)
{
    if (outcome) {
        memset(outcome, 0, sizeof(*outcome));
        outcome->delivery_session_id = delivery_session_id;
        outcome->previous_state = expected_state;
        outcome->current_state = expected_state;
        outcome->error = ESP_ERR_INVALID_ARG;
    }
    if (delivery_session_id == 0U || expected_delivery_revision == 0U ||
        !d1l_dm_delivery_state_valid(expected_state) ||
        !d1l_dm_delivery_state_valid(next_state) ||
        !d1l_dm_delivery_reason_valid(reason)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_store_initialized();
    if (ret != ESP_OK) {
        if (outcome) {
            outcome->error = ret;
        }
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    d1l_dm_entry_t *entry = NULL;
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[i].delivery_session_id == delivery_session_id) {
            entry = &s_entries[i];
            break;
        }
    }
    if (!entry) {
        d1l_store_lock_give(&s_store_lock);
        if (outcome) {
            outcome->error = ESP_ERR_NOT_FOUND;
        }
        return ESP_ERR_NOT_FOUND;
    }
    if (outcome) {
        outcome->row_seq = entry->seq;
        outcome->previous_state = entry->delivery_state;
        outcome->current_state = entry->delivery_state;
        outcome->retry_count = entry->delivery_retry_count;
        outcome->attempt = entry->attempt;
        outcome->ack_hash = entry->ack_hash;
        outcome->delivery_revision = entry->delivery_revision;
    }
    if (rebind_retry_ack &&
        (expected_state != D1L_DM_DELIVERY_RETRY_WAIT ||
         next_state != D1L_DM_DELIVERY_RETRY_TX ||
         reason != D1L_DM_DELIVERY_REASON_RETRY_STARTED ||
         error != ESP_OK)) {
        d1l_store_lock_give(&s_store_lock);
        if (outcome) {
            outcome->error = ESP_ERR_INVALID_STATE;
        }
        return ESP_ERR_INVALID_STATE;
    }
    const bool persistence_retry =
        next_state == D1L_DM_DELIVERY_ACKNOWLEDGED &&
        reason == D1L_DM_DELIVERY_REASON_ACK_RECEIVED && error == ESP_OK &&
        expected_delivery_revision != UINT32_MAX &&
        entry->delivery_state == D1L_DM_DELIVERY_ACKNOWLEDGED &&
        entry->delivery_reason == D1L_DM_DELIVERY_REASON_ACK_RECEIVED &&
        entry->delivery_last_error == ESP_OK &&
        entry->delivery_revision == expected_delivery_revision + 1U &&
        entry->acked && entry->delivered &&
        d1l_dm_delivery_transition_allowed(expected_state, next_state);
    if (persistence_retry) {
        if (outcome) {
            outcome->persistence_retry = true;
            outcome->previous_state = expected_state;
        }
        d1l_store_lock_give(&s_store_lock);
        ret = d1l_dm_store_flush();
        if (outcome) {
            outcome->durable = ret == ESP_OK;
            outcome->error = ret;
        }
        return ret;
    }

    d1l_dm_ack_persistence_pending_t *ack_pending = NULL;
    if (next_state == D1L_DM_DELIVERY_ACKNOWLEDGED) {
        ack_pending = reserve_ack_persistence_pending_locked(entry);
        if (!ack_pending) {
            d1l_store_lock_give(&s_store_lock);
            if (outcome) {
                outcome->error = ESP_ERR_NO_MEM;
            }
            return ESP_ERR_NO_MEM;
        }
    }
    ret = apply_delivery_transition_locked(
        entry, expected_state, expected_delivery_revision, next_state,
        reason, error);
    if (ret != ESP_OK) {
        if (ack_pending) {
            memset(ack_pending, 0, sizeof(*ack_pending));
        }
        d1l_store_lock_give(&s_store_lock);
        if (outcome) {
            outcome->error = ret;
        }
        return ret;
    }
    if (rebind_retry_ack) {
        entry->ack_hash = retry_ack_hash;
    }
    if (s_content_revision < UINT32_MAX) {
        s_content_revision++;
    }
    s_state_revision++;
    s_mutated_since_init = true;
    s_device_lineage_authoritative = true;
    s_sd_primary_dirty = true;
    s_nvs_fallback_dirty = true;
    if (outcome) {
        outcome->changed = true;
        outcome->current_state = entry->delivery_state;
        outcome->retry_count = entry->delivery_retry_count;
        outcome->attempt = entry->attempt;
        outcome->ack_hash = entry->ack_hash;
        outcome->delivery_revision = entry->delivery_revision;
    }
    d1l_store_lock_give(&s_store_lock);

    ret = d1l_dm_store_flush();
    if (outcome) {
        outcome->durable = ret == ESP_OK;
        outcome->error = ret;
    }
    return ret;
}

esp_err_t d1l_dm_store_transition_delivery(
    uint64_t delivery_session_id,
    d1l_dm_delivery_state_t expected_state,
    uint32_t expected_delivery_revision,
    d1l_dm_delivery_state_t next_state,
    d1l_dm_delivery_reason_t reason, esp_err_t error,
    d1l_dm_delivery_transition_outcome_t *outcome)
{
    return transition_delivery_internal(
        delivery_session_id, expected_state, expected_delivery_revision,
        next_state, reason, error, false, 0U, outcome);
}

esp_err_t d1l_dm_store_transition_delivery_retry(
    uint64_t delivery_session_id, uint32_t expected_delivery_revision,
    uint32_t retry_ack_hash,
    d1l_dm_delivery_transition_outcome_t *outcome)
{
    return transition_delivery_internal(
        delivery_session_id, D1L_DM_DELIVERY_RETRY_WAIT,
        expected_delivery_revision, D1L_DM_DELIVERY_RETRY_TX,
        D1L_DM_DELIVERY_REASON_RETRY_STARTED, ESP_OK, true,
        retry_ack_hash, outcome);
}

esp_err_t d1l_dm_store_mark_acked(uint32_t ack_hash,
                                  d1l_dm_entry_t *out_entry)
{
    if (ack_hash == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_store_initialized();
    if (ret != ESP_OK) {
        return ret;
    }

    uint64_t delivery_session_id = 0U;
    uint32_t delivery_revision = 0U;
    d1l_dm_delivery_state_t delivery_state =
        D1L_DM_DELIVERY_NOT_APPLICABLE;
    d1l_store_lock_take(&s_store_lock);
    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_dm_entry_t *entry = &s_entries[i];
        if (entry->ack_hash == ack_hash &&
            strncmp(entry->direction, "tx", sizeof(entry->direction)) == 0) {
            delivery_session_id = entry->delivery_session_id;
            delivery_revision = entry->delivery_revision;
            delivery_state = entry->delivery_state;
            const d1l_dm_ack_persistence_pending_t *pending =
                find_ack_persistence_pending_locked(
                    entry->delivery_session_id, entry->delivery_revision);
            if (pending) {
                delivery_revision = pending->previous_revision;
                delivery_state = pending->previous_state;
            }
            break;
        }
    }
    d1l_store_lock_give(&s_store_lock);
    if (delivery_session_id == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    if (delivery_state != D1L_DM_DELIVERY_ACKNOWLEDGED) {
        d1l_dm_delivery_transition_outcome_t outcome = {0};
        ret = d1l_dm_store_transition_delivery(
            delivery_session_id, delivery_state, delivery_revision,
            D1L_DM_DELIVERY_ACKNOWLEDGED,
            D1L_DM_DELIVERY_REASON_ACK_RECEIVED, ESP_OK, &outcome);
    }

    d1l_store_lock_take(&s_store_lock);
    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_dm_entry_t *entry = &s_entries[i];
        if (entry->delivery_session_id == delivery_session_id) {
            if (out_entry) {
                *out_entry = *entry;
                project_ack_persistence_truth_locked(out_entry);
            }
            break;
        }
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

static d1l_dm_store_stats_t dm_store_stats_locked(
    d1l_retained_blob_store_backend_state_t backend_state)
{
    d1l_dm_store_stats_t stats = {
        .next_seq = s_next_seq,
        .total_written = s_total_written,
        .dropped_oldest = s_dropped_oldest,
        .epoch = s_epoch,
        .content_revision = s_content_revision,
        .clear_lineage = s_clear_lineage,
        .persistence_commit_count = s_persistence_commit_count,
        .persistence_fail_count = s_persistence_fail_count,
        .persistence_stale_snapshot_count =
            s_persistence_stale_snapshot_count,
        .sd_primary_commit_count = s_sd_primary_commit_count,
        .sd_primary_fail_count = s_sd_primary_fail_count,
        .sd_backend_generation = backend_state.generation,
        .sd_primary_last_error = s_sd_primary_last_error,
        .nvs_fallback_commit_count = s_nvs_fallback_commit_count,
        .nvs_fallback_fail_count = s_nvs_fallback_fail_count,
        .nvs_fallback_last_error = s_nvs_fallback_last_error,
        .persistence_revision = s_state_revision,
        .count = s_count,
        .capacity = D1L_DM_STORE_CAPACITY,
        .loaded = s_loaded,
        .persistence_dirty = persistence_dirty_locked(backend_state.enabled),
        .sd_primary_required = backend_state.enabled,
        .sd_primary_dirty = s_sd_primary_dirty,
        .sd_primary_reconcile_pending = s_sd_reconcile_pending,
        .nvs_fallback_dirty = s_nvs_fallback_dirty,
    };
    return stats;
}

d1l_dm_store_stats_t d1l_dm_store_stats(void)
{
    d1l_retained_blob_store_backend_state_t backend_state = {0};
    (void)d1l_retained_blob_store_backend_state(D1L_DM_STORE_ID,
                                                &backend_state);
    d1l_store_lock_take(&s_store_lock);
    const d1l_dm_store_stats_t stats = dm_store_stats_locked(backend_state);
    d1l_store_lock_give(&s_store_lock);
    return stats;
}

size_t d1l_dm_store_copy_recent_page(d1l_dm_entry_t *out_entries,
                                     size_t max_entries,
                                     size_t skip_newest,
                                     size_t *out_total_matches)
{
    if (out_total_matches) {
        *out_total_matches = 0U;
    }
    if (!out_entries || max_entries == 0U) {
        return 0U;
    }

    d1l_store_lock_take(&s_store_lock);
    if (s_count == 0U && !s_volatile_valid) {
        d1l_store_lock_give(&s_store_lock);
        return 0U;
    }
    const size_t visible_count = s_count + (s_volatile_valid ? 1U : 0U);
    if (out_total_matches) {
        *out_total_matches = visible_count;
    }
    if (skip_newest >= visible_count) {
        d1l_store_lock_give(&s_store_lock);
        return 0U;
    }
    const size_t available = visible_count - skip_newest;
    const size_t copied = available < max_entries ? available : max_entries;
    const size_t first = visible_count - skip_newest - copied;
    const size_t oldest = s_count == 0U ? 0U :
        (s_head + D1L_DM_STORE_CAPACITY - s_count) % D1L_DM_STORE_CAPACITY;
    for (size_t i = 0U; i < copied; ++i) {
        const size_t visible_index = first + i;
        out_entries[i] = visible_index < s_count ?
            s_entries[(oldest + visible_index) % D1L_DM_STORE_CAPACITY] :
            s_volatile_entry;
        project_ack_persistence_truth_locked(&out_entries[i]);
    }
    d1l_store_lock_give(&s_store_lock);
    return copied;
}

size_t d1l_dm_store_copy_recent(d1l_dm_entry_t *out_entries,
                                size_t max_entries)
{
    return d1l_dm_store_copy_recent_page(out_entries, max_entries, 0U, NULL);
}

size_t d1l_dm_store_copy_recent_page_snapshot(
    d1l_dm_entry_t *out_entries, size_t max_entries, size_t skip_newest,
    size_t *out_total_matches, d1l_dm_store_stats_t *out_stats,
    uint32_t *out_volatile_seq)
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
    (void)d1l_retained_blob_store_backend_state(D1L_DM_STORE_ID,
                                                &backend_state);
    d1l_store_lock_take(&s_store_lock);
    const size_t visible_count = s_count + (s_volatile_valid ? 1U : 0U);
    if (out_total_matches) {
        *out_total_matches = visible_count;
    }
    if (s_volatile_valid) {
        *out_volatile_seq = s_volatile_entry.seq;
    }
    size_t copied = 0U;
    if (skip_newest < visible_count) {
        const size_t available = visible_count - skip_newest;
        copied = available < max_entries ? available : max_entries;
        const size_t first = visible_count - skip_newest - copied;
        const size_t oldest = s_count == 0U ? 0U :
            (s_head + D1L_DM_STORE_CAPACITY - s_count) %
                D1L_DM_STORE_CAPACITY;
        for (size_t i = 0U; i < copied; ++i) {
            const size_t visible_index = first + i;
            out_entries[i] = visible_index < s_count ?
                s_entries[(oldest + visible_index) %
                          D1L_DM_STORE_CAPACITY] :
                s_volatile_entry;
            project_ack_persistence_truth_locked(&out_entries[i]);
        }
    }
    *out_stats = dm_store_stats_locked(backend_state);
    d1l_store_lock_give(&s_store_lock);
    return copied;
}

size_t d1l_dm_store_query_thread_page(const char *contact_fingerprint,
                                      d1l_dm_entry_t *out_entries,
                                      size_t max_entries,
                                      size_t skip_newest,
                                      const char *query,
                                      size_t *out_total_matches)
{
    if (out_total_matches) {
        *out_total_matches = 0U;
    }
    if (!contact_fingerprint || contact_fingerprint[0] == '\0' ||
        !out_entries || max_entries == 0U) {
        return 0U;
    }

    d1l_store_lock_take(&s_store_lock);
    if (s_count == 0U && !s_volatile_valid) {
        d1l_store_lock_give(&s_store_lock);
        return 0U;
    }
    size_t total_matches = 0U;
    const size_t oldest = s_count == 0U ? 0U :
        (s_head + D1L_DM_STORE_CAPACITY - s_count) % D1L_DM_STORE_CAPACITY;
    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_dm_entry_t *entry =
            &s_entries[(oldest + i) % D1L_DM_STORE_CAPACITY];
        if (strncmp(entry->contact_fingerprint, contact_fingerprint,
                    sizeof(entry->contact_fingerprint)) == 0 &&
            dm_entry_matches_query(entry, query)) {
            total_matches++;
        }
    }
    const bool volatile_matches = s_volatile_valid &&
        strncmp(s_volatile_entry.contact_fingerprint, contact_fingerprint,
                sizeof(s_volatile_entry.contact_fingerprint)) == 0 &&
        dm_entry_matches_query(&s_volatile_entry, query);
    if (volatile_matches) {
        total_matches++;
    }
    if (out_total_matches) {
        *out_total_matches = total_matches;
    }
    if (skip_newest >= total_matches) {
        d1l_store_lock_give(&s_store_lock);
        return 0U;
    }

    const size_t available = total_matches - skip_newest;
    const size_t copied = available < max_entries ? available : max_entries;
    const size_t first_match = total_matches - skip_newest - copied;
    const size_t last_match = first_match + copied;
    size_t match_index = 0U;
    size_t out_index = 0U;
    for (size_t i = 0U; i < s_count && out_index < copied; ++i) {
        const d1l_dm_entry_t *entry =
            &s_entries[(oldest + i) % D1L_DM_STORE_CAPACITY];
        if (strncmp(entry->contact_fingerprint, contact_fingerprint,
                    sizeof(entry->contact_fingerprint)) != 0 ||
            !dm_entry_matches_query(entry, query)) {
            continue;
        }
        if (match_index >= first_match && match_index < last_match) {
            out_entries[out_index++] = *entry;
            project_ack_persistence_truth_locked(
                &out_entries[out_index - 1U]);
        }
        match_index++;
    }
    if (volatile_matches && out_index < copied &&
        match_index >= first_match && match_index < last_match) {
        out_entries[out_index++] = s_volatile_entry;
        project_ack_persistence_truth_locked(
            &out_entries[out_index - 1U]);
    }
    d1l_store_lock_give(&s_store_lock);
    return out_index;
}

size_t d1l_dm_store_copy_thread_page(const char *contact_fingerprint,
                                     d1l_dm_entry_t *out_entries,
                                     size_t max_entries,
                                     size_t skip_newest,
                                     size_t *out_total_matches)
{
    return d1l_dm_store_query_thread_page(
        contact_fingerprint, out_entries, max_entries, skip_newest, NULL,
        out_total_matches);
}

size_t d1l_dm_store_copy_thread_page_snapshot(
    const char *contact_fingerprint, d1l_dm_entry_t *out_entries,
    size_t max_entries, size_t skip_newest, size_t *out_total_matches,
    d1l_dm_store_stats_t *out_stats, uint32_t *out_volatile_seq)
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
    if (!contact_fingerprint || contact_fingerprint[0] == '\0' ||
        !out_entries || max_entries == 0U || !out_stats ||
        !out_volatile_seq) {
        return 0U;
    }

    d1l_retained_blob_store_backend_state_t backend_state = {0};
    (void)d1l_retained_blob_store_backend_state(D1L_DM_STORE_ID,
                                                &backend_state);
    d1l_store_lock_take(&s_store_lock);
    const size_t oldest = s_count == 0U ? 0U :
        (s_head + D1L_DM_STORE_CAPACITY - s_count) %
            D1L_DM_STORE_CAPACITY;
    size_t total_matches = 0U;
    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_dm_entry_t *entry =
            &s_entries[(oldest + i) % D1L_DM_STORE_CAPACITY];
        if (strncmp(entry->contact_fingerprint, contact_fingerprint,
                    sizeof(entry->contact_fingerprint)) == 0) {
            total_matches++;
        }
    }
    const bool volatile_matches =
        s_volatile_valid &&
        strncmp(s_volatile_entry.contact_fingerprint, contact_fingerprint,
                sizeof(s_volatile_entry.contact_fingerprint)) == 0;
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
        for (size_t i = 0U; i < s_count; ++i) {
            const d1l_dm_entry_t *entry =
                &s_entries[(oldest + i) % D1L_DM_STORE_CAPACITY];
            if (strncmp(entry->contact_fingerprint, contact_fingerprint,
                        sizeof(entry->contact_fingerprint)) != 0) {
                continue;
            }
            if (match_index >= first_match && match_index < last_match) {
                out_entries[match_index - first_match] = *entry;
                project_ack_persistence_truth_locked(
                    &out_entries[match_index - first_match]);
            }
            match_index++;
        }
        if (volatile_matches && match_index >= first_match &&
            match_index < last_match) {
            out_entries[match_index - first_match] = s_volatile_entry;
            project_ack_persistence_truth_locked(
                &out_entries[match_index - first_match]);
        }
    }
    *out_stats = dm_store_stats_locked(backend_state);
    d1l_store_lock_give(&s_store_lock);
    return copied;
}

size_t d1l_dm_store_copy_thread(const char *contact_fingerprint,
                                d1l_dm_entry_t *out_entries,
                                size_t max_entries)
{
    return d1l_dm_store_copy_thread_page(contact_fingerprint, out_entries,
                                         max_entries, 0U, NULL);
}
