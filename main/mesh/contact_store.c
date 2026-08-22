#include "contact_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

#include "mesh/contact_uri.h"
#include "mesh/meshcore_wire.h"
#include "mesh/store_lock.h"
#include "storage/retained_blob_store.h"

#define D1L_CONTACT_STORE_KEY "contacts"
#define D1L_CONTACT_STORE_LEGACY_ALIAS_LEN 24U
#define D1L_CONTACT_STORE_LEGACY_TYPE_LEN 8U
#define D1L_CONTACT_STORE_SCHEMA_V1 1U
#define D1L_CONTACT_STORE_SCHEMA_V2 2U
#define D1L_CONTACT_STORE_SCHEMA_V3 3U
#define D1L_CONTACT_STORE_SCHEMA_V4 4U
#define D1L_CONTACT_STORE_SCHEMA_V5 5U
#define D1L_CONTACT_STORE_SCHEMA_V6 6U
#define D1L_CONTACT_STORE_SCHEMA_V7 7U
#define D1L_CONTACT_STORE_SCHEMA 8U
#define D1L_CONTACT_STORE_LEGACY_CAPACITY 16U

typedef struct {
    uint32_t seq;
    uint32_t created_ms;
    uint32_t updated_ms;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char alias[D1L_CONTACT_STORE_LEGACY_ALIAS_LEN];
    char heard_name[D1L_HEARD_NODE_NAME_LEN];
    char type[D1L_CONTACT_STORE_LEGACY_TYPE_LEN];
    int last_rssi_dbm;
    int last_snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool favorite;
    bool muted;
} d1l_contact_entry_v1_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_contact_entry_v1_t entries[D1L_CONTACT_STORE_LEGACY_CAPACITY];
} d1l_contact_store_blob_v1_t;

typedef struct {
    uint32_t seq;
    uint32_t created_ms;
    uint32_t updated_ms;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    char alias[D1L_CONTACT_STORE_LEGACY_ALIAS_LEN];
    char heard_name[D1L_HEARD_NODE_NAME_LEN];
    char type[D1L_CONTACT_STORE_LEGACY_TYPE_LEN];
    int last_rssi_dbm;
    int last_snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool favorite;
    bool muted;
} d1l_contact_entry_v2_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_contact_entry_v2_t entries[D1L_CONTACT_STORE_LEGACY_CAPACITY];
} d1l_contact_store_blob_v2_t;

/* Schema v3 predates canonical advert roles and the 9-byte repeater string. */
typedef struct {
    uint32_t seq;
    uint32_t created_ms;
    uint32_t updated_ms;
    uint32_t out_path_updated_ms;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    char alias[D1L_CONTACT_STORE_LEGACY_ALIAS_LEN];
    char heard_name[D1L_HEARD_NODE_NAME_LEN];
    char type[D1L_CONTACT_STORE_LEGACY_TYPE_LEN];
    int last_rssi_dbm;
    int last_snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool out_path_valid;
    uint8_t out_path_len;
    uint8_t out_path[D1L_CONTACT_OUT_PATH_MAX];
    bool favorite;
    bool muted;
} d1l_contact_entry_v3_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_contact_entry_v3_t entries[D1L_CONTACT_STORE_LEGACY_CAPACITY];
} d1l_contact_store_blob_v3_t;

/* Schema v4 is the last layout before explicit identity provenance. */
typedef struct {
    uint32_t seq;
    uint32_t created_ms;
    uint32_t updated_ms;
    uint32_t out_path_updated_ms;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    char alias[D1L_CONTACT_STORE_LEGACY_ALIAS_LEN];
    char heard_name[D1L_HEARD_NODE_NAME_LEN];
    char type[D1L_NODE_TYPE_LEN];
    int last_rssi_dbm;
    int last_snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool out_path_valid;
    uint8_t out_path_len;
    uint8_t out_path[D1L_CONTACT_OUT_PATH_MAX];
    bool favorite;
    bool muted;
} d1l_contact_entry_v4_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_contact_entry_v4_t entries[D1L_CONTACT_STORE_LEGACY_CAPACITY];
} d1l_contact_store_blob_v4_t;

/* Schema v5 is the frozen provenance layout with a 23-byte name capacity. */
typedef struct {
    uint32_t seq;
    uint32_t created_ms;
    uint32_t updated_ms;
    uint32_t out_path_updated_ms;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    char alias[D1L_CONTACT_STORE_LEGACY_ALIAS_LEN];
    char heard_name[D1L_HEARD_NODE_NAME_LEN];
    char type[D1L_NODE_TYPE_LEN];
    int last_rssi_dbm;
    int last_snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool out_path_valid;
    uint8_t out_path_len;
    uint8_t out_path[D1L_CONTACT_OUT_PATH_MAX];
    bool favorite;
    bool muted;
    uint8_t verification_source;
    uint32_t verified_at_ms;
    uint32_t signed_advert_timestamp;
    uint32_t last_heard_ms;
} d1l_contact_entry_v5_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_contact_entry_v5_t entries[D1L_CONTACT_STORE_LEGACY_CAPACITY];
} d1l_contact_store_blob_v5_t;

/* Schema v6 is the last layout before canonical retained path lifecycle. */
typedef struct {
    uint32_t seq;
    uint32_t created_ms;
    uint32_t updated_ms;
    uint32_t out_path_updated_ms;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    char alias[D1L_CONTACT_ALIAS_LEN];
    char heard_name[D1L_HEARD_NODE_NAME_LEN];
    char type[D1L_NODE_TYPE_LEN];
    int last_rssi_dbm;
    int last_snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool out_path_valid;
    uint8_t out_path_len;
    uint8_t out_path[D1L_CONTACT_OUT_PATH_MAX];
    bool favorite;
    bool muted;
    uint8_t verification_source;
    uint32_t verified_at_ms;
    uint32_t signed_advert_timestamp;
    uint32_t last_heard_ms;
} d1l_contact_entry_v6_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_contact_entry_v6_t entries[D1L_CONTACT_STORE_LEGACY_CAPACITY];
} d1l_contact_store_blob_v6_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_contact_entry_t entries[D1L_CONTACT_STORE_LEGACY_CAPACITY];
} d1l_contact_store_blob_v7_t;

_Static_assert(sizeof(d1l_contact_entry_v1_t) == 100U,
               "contact schema v1 layout changed");
_Static_assert(sizeof(d1l_contact_entry_v2_t) == 164U,
               "contact schema v2 layout changed");
_Static_assert(sizeof(d1l_contact_entry_v3_t) == 236U,
               "contact schema v3 layout changed");
_Static_assert(sizeof(d1l_contact_entry_v4_t) == 236U,
               "contact schema v4 layout changed");
_Static_assert(sizeof(d1l_contact_entry_v5_t) == 248U,
                "contact schema v5 layout changed");
_Static_assert(sizeof(d1l_contact_entry_v6_t) == 256U,
                "contact schema v6 layout changed");
_Static_assert(sizeof(d1l_contact_entry_t) == 280U,
                "contact schema v7 layout changed");
_Static_assert(D1L_CONTACT_STORE_LEGACY_CAPACITY <
                   D1L_CONTACT_STORE_CAPACITY,
               "contact schema v8 must expand retained capacity");
_Static_assert(offsetof(d1l_contact_entry_v1_t, last_rssi_dbm) == 88U,
               "contact schema v1 type offset changed");
_Static_assert(offsetof(d1l_contact_entry_v2_t, last_rssi_dbm) == 152U,
               "contact schema v2 type offset changed");
_Static_assert(offsetof(d1l_contact_entry_v3_t, last_rssi_dbm) == 156U,
               "contact schema v3 type offset changed");
_Static_assert(offsetof(d1l_contact_entry_v4_t, last_rssi_dbm) == 156U,
               "contact schema v4 type offset changed");

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_contact_entry_t entries[D1L_CONTACT_STORE_CAPACITY];
} d1l_contact_store_blob_t;

typedef enum {
    D1L_CONTACT_MUTATION_AUTHORITY_NONE = 0,
    D1L_CONTACT_MUTATION_AUTHORITY_LOCAL,
    D1L_CONTACT_MUTATION_AUTHORITY_EXPLICIT_CLEAR,
} d1l_contact_mutation_authority_t;

static d1l_contact_entry_t
    s_entries[D1L_CONTACT_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static size_t s_count;
static uint32_t s_next_seq = 1;
static uint32_t s_total_written;
static uint32_t s_dropped_oldest;
static bool s_loaded;
static d1l_contact_store_blob_t s_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_contact_store_blob_t s_sd_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_contact_store_blob_t s_rollback_scratch EXT_RAM_BSS_ATTR;
static d1l_contact_store_blob_t s_persist_snapshot EXT_RAM_BSS_ATTR;
static char s_deleted_fingerprints[D1L_CONTACT_STORE_CAPACITY]
                                  [D1L_NODE_FINGERPRINT_LEN] EXT_RAM_BSS_ATTR;
static char s_rollback_deleted_fingerprints[D1L_CONTACT_STORE_CAPACITY]
                                           [D1L_NODE_FINGERPRINT_LEN]
                                               EXT_RAM_BSS_ATTR;
static char s_touched_fingerprints[D1L_CONTACT_STORE_CAPACITY]
                                  [D1L_NODE_FINGERPRINT_LEN] EXT_RAM_BSS_ATTR;
static char s_rollback_touched_fingerprints[D1L_CONTACT_STORE_CAPACITY]
                                           [D1L_NODE_FINGERPRINT_LEN]
                                               EXT_RAM_BSS_ATTR;
static size_t s_deleted_fingerprint_count;
static size_t s_rollback_deleted_fingerprint_count;
static size_t s_touched_fingerprint_count;
static size_t s_rollback_touched_fingerprint_count;
static d1l_contact_mutation_authority_t s_mutation_authority;
static d1l_contact_mutation_authority_t s_rollback_mutation_authority;
static uint32_t s_accepted_sd_backend_generation;
static bool s_sd_reconcile_pending;
static bool s_last_persist_durable;
static bool s_last_persist_wrote;
static uint32_t s_persistence_revision;
static uint32_t s_persistence_commit_count;
static uint32_t s_persistence_coalesced_count;
static uint32_t s_persistence_fail_count;
static esp_err_t s_persistence_last_error;
static bool s_persistence_dirty;
static uint32_t s_persistence_dirty_since_ms;
static d1l_store_lock_t s_store_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_persist_io_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_deferred_flush_lock = D1L_STORE_LOCK_INITIALIZER;

static bool fixed_hex_string_valid(const char *value, size_t hex_chars);
static bool fixed_hex_strings_equal(const char *left, const char *right,
                                    size_t hex_chars);
static int find_unique_index_by_fingerprint_hex(const char *fingerprint,
                                                bool *out_ambiguous);
static bool blob_is_valid(const d1l_contact_store_blob_t *blob, size_t len);

static void mark_migrated_verification(d1l_contact_entry_t *entry)
{
    if (!entry) {
        return;
    }
    entry->verification_source = D1L_CONTACT_VERIFICATION_NONE;
    entry->verified_at_ms = 0U;
    entry->signed_advert_timestamp = 0U;
    entry->last_heard_ms = 0U;
    if (fixed_hex_string_valid(entry->fingerprint,
                               D1L_NODE_FINGERPRINT_LEN - 1U) &&
        fixed_hex_string_valid(entry->public_key_hex,
                               D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U) &&
        fixed_hex_strings_equal(entry->fingerprint, entry->public_key_hex,
                                D1L_NODE_FINGERPRINT_LEN - 1U) &&
        d1l_contact_store_meshcore_type_id(entry->type) != 0U) {
        entry->verification_source =
            D1L_CONTACT_VERIFICATION_MIGRATED_SIGNED_ADVERT;
        entry->verified_at_ms = entry->updated_ms;
    }
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

static void migrate_legacy_advert_type(
    char dest[D1L_NODE_TYPE_LEN],
    const char legacy_type[D1L_CONTACT_STORE_LEGACY_TYPE_LEN])
{
    char legacy[D1L_CONTACT_STORE_LEGACY_TYPE_LEN + 1U] = {0};
    memcpy(legacy, legacy_type, D1L_CONTACT_STORE_LEGACY_TYPE_LEN);
    const char *canonical = "unknown";
    if (strcmp(legacy, "chat") == 0) {
        canonical = "chat";
    } else if (strcmp(legacy, "room") == 0 ||
               strcmp(legacy, "repeater") == 0) {
        /* The legacy advert decoder mapped upstream type 2 to room. */
        canonical = "repeater";
    }
    /* Legacy sensor is ambiguous: the old decoder used it for upstream room
     * type 3 and sensor type 4. Keep it unknown until a fresh signed advert. */
    snprintf(dest, D1L_NODE_TYPE_LEN, "%s", canonical);
}

static void clear_ram(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
    s_next_seq = 1;
    s_total_written = 0;
    s_dropped_oldest = 0;
}

static void reset_persistence_state(uint32_t backend_generation)
{
    __atomic_store_n(&s_persistence_revision, 0U, __ATOMIC_RELEASE);
    s_persistence_commit_count = 0U;
    s_persistence_coalesced_count = 0U;
    s_persistence_fail_count = 0U;
    s_persistence_last_error = ESP_OK;
    s_persistence_dirty = false;
    s_persistence_dirty_since_ms = 0U;
    s_accepted_sd_backend_generation = backend_generation;
    s_sd_reconcile_pending = false;
    s_mutation_authority = D1L_CONTACT_MUTATION_AUTHORITY_NONE;
    s_deleted_fingerprint_count = 0U;
    memset(s_deleted_fingerprints, 0, sizeof(s_deleted_fingerprints));
    s_touched_fingerprint_count = 0U;
    memset(s_touched_fingerprints, 0, sizeof(s_touched_fingerprints));
    s_last_persist_durable = false;
    s_last_persist_wrote = false;
}

static esp_err_t reserve_persistence_revision_locked(bool count_write)
{
    const uint32_t revision = __atomic_load_n(
        &s_persistence_revision, __ATOMIC_ACQUIRE);
    if (revision == UINT32_MAX ||
        (count_write && s_total_written == UINT32_MAX)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (count_write) {
        s_total_written++;
    }
    __atomic_store_n(
        &s_persistence_revision, revision + 1U, __ATOMIC_RELEASE);
    if (s_mutation_authority == D1L_CONTACT_MUTATION_AUTHORITY_NONE) {
        s_mutation_authority = D1L_CONTACT_MUTATION_AUTHORITY_LOCAL;
    }
    return ESP_OK;
}

static esp_err_t reserve_sequenced_mutation_locked(void)
{
    /* next_seq is the next value to assign. UINT32_MAX cannot be consumed
     * because incrementing it would persist zero and make the next boot reject
     * an otherwise valid blob. Preserve both RAM and durable truth instead. */
    if (s_next_seq == UINT32_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    return reserve_persistence_revision_locked(true);
}

static void mark_deferred_persistence_locked(uint32_t now_ms)
{
    if (!s_persistence_dirty) {
        s_persistence_dirty_since_ms = now_ms;
    }
    s_persistence_dirty = true;
}

static void fill_blob(d1l_contact_store_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->schema = D1L_CONTACT_STORE_SCHEMA;
    blob->next_seq = s_next_seq;
    blob->total_written = s_total_written;
    blob->dropped_oldest = s_dropped_oldest;
    blob->count = (uint32_t)s_count;
    memcpy(blob->entries, s_entries, sizeof(s_entries));
}

static void capture_rollback_state(void)
{
    fill_blob(&s_rollback_scratch);
    s_rollback_mutation_authority = s_mutation_authority;
    s_rollback_deleted_fingerprint_count = s_deleted_fingerprint_count;
    memcpy(s_rollback_deleted_fingerprints, s_deleted_fingerprints,
           sizeof(s_deleted_fingerprints));
    s_rollback_touched_fingerprint_count = s_touched_fingerprint_count;
    memcpy(s_rollback_touched_fingerprints, s_touched_fingerprints,
           sizeof(s_touched_fingerprints));
}

static void restore_rollback_authority(void)
{
    s_mutation_authority = s_rollback_mutation_authority;
    s_deleted_fingerprint_count = s_rollback_deleted_fingerprint_count;
    memcpy(s_deleted_fingerprints, s_rollback_deleted_fingerprints,
           sizeof(s_deleted_fingerprints));
    s_touched_fingerprint_count = s_rollback_touched_fingerprint_count;
    memcpy(s_touched_fingerprints, s_rollback_touched_fingerprints,
           sizeof(s_touched_fingerprints));
}

static void release_durable_mutation_authority_locked(void)
{
    s_mutation_authority = D1L_CONTACT_MUTATION_AUTHORITY_NONE;
    s_deleted_fingerprint_count = 0U;
    memset(s_deleted_fingerprints, 0, sizeof(s_deleted_fingerprints));
    s_touched_fingerprint_count = 0U;
    memset(s_touched_fingerprints, 0, sizeof(s_touched_fingerprints));
}

static void restore_blob(const d1l_contact_store_blob_t *blob)
{
    if (!blob) {
        return;
    }
    s_next_seq = blob->next_seq;
    s_total_written = blob->total_written;
    s_dropped_oldest = blob->dropped_oldest;
    s_count = blob->count <= D1L_CONTACT_STORE_CAPACITY ? blob->count : 0;
    memset(s_entries, 0, sizeof(s_entries));
    memcpy(s_entries, blob->entries, s_count * sizeof(s_entries[0]));
}

static bool contact_entries_same_identity(
    const d1l_contact_entry_t *left, const d1l_contact_entry_t *right,
    bool *out_conflict)
{
    if (out_conflict) {
        *out_conflict = false;
    }
    if (!left || !right) {
        return false;
    }
    const bool left_key_valid = fixed_hex_string_valid(
        left->public_key_hex, D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U);
    const bool right_key_valid = fixed_hex_string_valid(
        right->public_key_hex, D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U);
    if (left_key_valid && right_key_valid &&
        fixed_hex_strings_equal(left->public_key_hex, right->public_key_hex,
                                D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U)) {
        return true;
    }
    const bool same_fingerprint =
        fixed_hex_string_valid(left->fingerprint,
                               D1L_NODE_FINGERPRINT_LEN - 1U) &&
        fixed_hex_string_valid(right->fingerprint,
                               D1L_NODE_FINGERPRINT_LEN - 1U) &&
        fixed_hex_strings_equal(left->fingerprint, right->fingerprint,
                                D1L_NODE_FINGERPRINT_LEN - 1U);
    if (same_fingerprint && left_key_valid && right_key_valid) {
        if (out_conflict) {
            *out_conflict = true;
        }
        return false;
    }
    return same_fingerprint;
}

static bool fingerprint_was_deleted_locked(const char *fingerprint)
{
    if (!fixed_hex_string_valid(fingerprint,
                                D1L_NODE_FINGERPRINT_LEN - 1U)) {
        return false;
    }
    for (size_t i = 0U; i < s_deleted_fingerprint_count; ++i) {
        if (fixed_hex_strings_equal(
                s_deleted_fingerprints[i], fingerprint,
                D1L_NODE_FINGERPRINT_LEN - 1U)) {
            return true;
        }
    }
    return false;
}

static void remember_deleted_fingerprint_locked(const char *fingerprint)
{
    if (s_mutation_authority ==
            D1L_CONTACT_MUTATION_AUTHORITY_EXPLICIT_CLEAR ||
        fingerprint_was_deleted_locked(fingerprint) ||
        s_deleted_fingerprint_count >= D1L_CONTACT_STORE_CAPACITY) {
        return;
    }
    snprintf(s_deleted_fingerprints[s_deleted_fingerprint_count],
             sizeof(s_deleted_fingerprints[0]), "%s", fingerprint);
    s_deleted_fingerprint_count++;
}

static bool fingerprint_was_touched_locked(const char *fingerprint)
{
    if (!fixed_hex_string_valid(fingerprint,
                                D1L_NODE_FINGERPRINT_LEN - 1U)) {
        return false;
    }
    for (size_t i = 0U; i < s_touched_fingerprint_count; ++i) {
        if (fixed_hex_strings_equal(
                s_touched_fingerprints[i], fingerprint,
                D1L_NODE_FINGERPRINT_LEN - 1U)) {
            return true;
        }
    }
    return false;
}

static void remember_touched_fingerprint_locked(const char *fingerprint)
{
    if (s_mutation_authority ==
            D1L_CONTACT_MUTATION_AUTHORITY_EXPLICIT_CLEAR ||
        fingerprint_was_touched_locked(fingerprint) ||
        s_touched_fingerprint_count >= D1L_CONTACT_STORE_CAPACITY ||
        !fixed_hex_string_valid(fingerprint,
                                D1L_NODE_FINGERPRINT_LEN - 1U)) {
        return;
    }
    snprintf(s_touched_fingerprints[s_touched_fingerprint_count],
             sizeof(s_touched_fingerprints[0]), "%s", fingerprint);
    s_touched_fingerprint_count++;
}

static void remember_all_current_fingerprints_locked(void)
{
    for (size_t i = 0U; i < s_count; ++i) {
        remember_touched_fingerprint_locked(s_entries[i].fingerprint);
    }
}

static bool contact_blobs_equivalent(const d1l_contact_store_blob_t *left,
                                     const d1l_contact_store_blob_t *right)
{
    return left && right &&
           left->schema == right->schema &&
           left->next_seq == right->next_seq &&
           left->total_written == right->total_written &&
           left->dropped_oldest == right->dropped_oldest &&
           left->count == right->count &&
           memcmp(left->entries, right->entries,
                  left->count * sizeof(left->entries[0])) == 0;
}

static uint32_t max_u32(uint32_t left, uint32_t right)
{
    return left > right ? left : right;
}

static esp_err_t merge_contact_blobs_locked(
    const d1l_contact_store_blob_t *local,
    const d1l_contact_store_blob_t *primary,
    d1l_contact_store_blob_t *out)
{
    if (!local || !primary || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->schema = D1L_CONTACT_STORE_SCHEMA;
    out->next_seq = max_u32(local->next_seq, primary->next_seq);
    out->total_written =
        max_u32(local->total_written, primary->total_written);
    out->dropped_oldest =
        max_u32(local->dropped_oldest, primary->dropped_oldest);

    for (size_t source = 0U; source < primary->count; ++source) {
        const d1l_contact_entry_t *candidate = &primary->entries[source];
        if (fingerprint_was_deleted_locked(candidate->fingerprint)) {
            continue;
        }
        out->entries[out->count++] = *candidate;
    }

    for (size_t source = 0U; source < local->count; ++source) {
        const d1l_contact_entry_t *candidate = &local->entries[source];
        if (!fingerprint_was_touched_locked(candidate->fingerprint)) {
            continue;
        }
        bool matched = false;
        for (size_t current = 0U; current < out->count; ++current) {
            bool conflict = false;
            if (contact_entries_same_identity(&out->entries[current],
                                              candidate, &conflict)) {
                out->entries[current] = *candidate;
                matched = true;
                break;
            }
            if (conflict) {
                return ESP_ERR_INVALID_STATE;
            }
        }
        if (matched) {
            continue;
        }
        if (out->count >= D1L_CONTACT_STORE_CAPACITY) {
            return ESP_ERR_NO_MEM;
        }
        out->entries[out->count++] = *candidate;
    }
    if (out->total_written < out->count) {
        out->total_written = out->count;
    }
    memset(&out->entries[out->count], 0,
           (D1L_CONTACT_STORE_CAPACITY - out->count) *
               sizeof(out->entries[0]));
    return ESP_OK;
}

static bool contact_sd_backend_generation_matches(uint32_t expected_generation)
{
    d1l_retained_blob_store_backend_state_t backend = {0};
    return d1l_retained_blob_store_backend_state(
               D1L_RETAINED_BLOB_STORE_CONTACTS, &backend) &&
           backend.enabled && backend.generation == expected_generation;
}

static esp_err_t read_current_sd_blob(uint32_t expected_generation,
                                      bool *out_found)
{
    if (!out_found ||
        !contact_sd_backend_generation_matches(expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_found = false;
    memset(&s_sd_blob_scratch, 0, sizeof(s_sd_blob_scratch));
    size_t len = sizeof(s_sd_blob_scratch);
    const esp_err_t ret = d1l_retained_blob_store_read_sd_primary(
        D1L_RETAINED_BLOB_STORE_CONTACTS, D1L_CONTACT_STORE_KEY,
        &s_sd_blob_scratch, &len);
    if (ret == ESP_ERR_NOT_FINISHED) {
        return ret;
    }
    if (ret == ESP_ERR_NOT_FOUND) {
        return contact_sd_backend_generation_matches(expected_generation) ?
            ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (ret != ESP_OK ||
        !blob_is_valid(&s_sd_blob_scratch, len) ||
        !contact_sd_backend_generation_matches(expected_generation)) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    *out_found = true;
    return ESP_OK;
}

static esp_err_t reconcile_sd_primary_locked(
    uint32_t expected_generation, bool *out_write_needed,
    bool *out_erase_needed)
{
    if (!out_write_needed || !out_erase_needed) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_write_needed = false;
    *out_erase_needed = false;

    bool found = false;
    esp_err_t ret = read_current_sd_blob(expected_generation, &found);
    if (ret != ESP_OK) {
        s_sd_reconcile_pending = true;
        return ret;
    }

    fill_blob(&s_blob_scratch);
    if (!found) {
        *out_write_needed =
            s_mutation_authority !=
                D1L_CONTACT_MUTATION_AUTHORITY_EXPLICIT_CLEAR &&
            (s_count > 0U ||
             s_mutation_authority == D1L_CONTACT_MUTATION_AUTHORITY_LOCAL);
        s_sd_reconcile_pending = false;
        return ESP_OK;
    }

    if (s_mutation_authority == D1L_CONTACT_MUTATION_AUTHORITY_NONE) {
        if (!contact_blobs_equivalent(&s_blob_scratch,
                                      &s_sd_blob_scratch)) {
            if (__atomic_load_n(&s_persistence_revision,
                                __ATOMIC_ACQUIRE) == UINT32_MAX) {
                s_sd_reconcile_pending = true;
                return ESP_ERR_INVALID_STATE;
            }
            restore_blob(&s_sd_blob_scratch);
            (void)__atomic_add_fetch(
                &s_persistence_revision, 1U, __ATOMIC_RELEASE);
        }
    } else if (s_mutation_authority ==
               D1L_CONTACT_MUTATION_AUTHORITY_EXPLICIT_CLEAR) {
        *out_erase_needed = true;
    } else {
        ret = merge_contact_blobs_locked(
            &s_blob_scratch, &s_sd_blob_scratch, &s_persist_snapshot);
        if (ret != ESP_OK) {
            s_sd_reconcile_pending = true;
            return ret;
        }
        const bool ram_changed =
            !contact_blobs_equivalent(&s_blob_scratch, &s_persist_snapshot);
        if (ram_changed) {
            if (__atomic_load_n(&s_persistence_revision,
                                __ATOMIC_ACQUIRE) == UINT32_MAX) {
                s_sd_reconcile_pending = true;
                return ESP_ERR_INVALID_STATE;
            }
            restore_blob(&s_persist_snapshot);
            (void)__atomic_add_fetch(
                &s_persistence_revision, 1U, __ATOMIC_RELEASE);
        }
        *out_write_needed =
            !contact_blobs_equivalent(&s_persist_snapshot,
                                      &s_sd_blob_scratch);
    }

    if (!contact_sd_backend_generation_matches(expected_generation)) {
        restore_blob(&s_blob_scratch);
        s_sd_reconcile_pending = true;
        return ESP_ERR_INVALID_STATE;
    }
    s_sd_reconcile_pending = false;
    return ESP_OK;
}

static esp_err_t persist_store(void)
{
    s_last_persist_durable = false;
    s_last_persist_wrote = false;

    d1l_store_lock_take(&s_persist_io_lock);
    d1l_retained_blob_store_backend_state_t backend = {0};
    if (!d1l_retained_blob_store_backend_state(
            D1L_RETAINED_BLOB_STORE_CONTACTS, &backend)) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (backend.generation != s_accepted_sd_backend_generation) {
        s_accepted_sd_backend_generation = backend.generation;
        s_sd_reconcile_pending = backend.enabled;
    }
    if (!backend.enabled) {
        s_sd_reconcile_pending = true;
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }

    bool write_needed =
        s_mutation_authority == D1L_CONTACT_MUTATION_AUTHORITY_LOCAL ||
        s_persistence_dirty;
    bool erase_needed =
        s_mutation_authority ==
            D1L_CONTACT_MUTATION_AUTHORITY_EXPLICIT_CLEAR &&
        s_count == 0U;
    if (s_sd_reconcile_pending) {
        const esp_err_t reconcile_ret = reconcile_sd_primary_locked(
            backend.generation, &write_needed, &erase_needed);
        if (reconcile_ret != ESP_OK) {
            d1l_store_lock_give(&s_persist_io_lock);
            return reconcile_ret;
        }
    }

    esp_err_t ret = ESP_OK;
    if (erase_needed) {
        ret = d1l_retained_blob_store_erase_sd_primary_guarded(
            D1L_RETAINED_BLOB_STORE_CONTACTS, D1L_CONTACT_STORE_KEY,
            backend.generation);
    } else if (write_needed) {
        fill_blob(&s_blob_scratch);
        if (!contact_sd_backend_generation_matches(backend.generation)) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            ret = d1l_retained_blob_store_write_sd_primary_guarded(
                D1L_RETAINED_BLOB_STORE_CONTACTS, D1L_CONTACT_STORE_KEY,
                &s_blob_scratch, sizeof(s_blob_scratch),
                backend.generation);
        }
    }
    if (ret == ESP_OK &&
        !contact_sd_backend_generation_matches(backend.generation)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        ret = d1l_retained_blob_store_erase_nvs_fallback(
            D1L_RETAINED_BLOB_STORE_CONTACTS, D1L_CONTACT_STORE_KEY);
    }
    if (ret != ESP_OK) {
        /* A guarded write/erase may have committed before reporting a cleanup
         * or transport error. Force an authoritative reread before any retry
         * so RAM is never allowed to guess which copy won. */
        s_sd_reconcile_pending = true;
    }
    s_last_persist_durable = ret == ESP_OK;
    s_last_persist_wrote = ret == ESP_OK && (write_needed || erase_needed);
    d1l_store_lock_give(&s_persist_io_lock);
    return ret;
}

static esp_err_t persist_store_or_rollback(const d1l_contact_store_blob_t *before)
{
    const bool dirty_before = s_persistence_dirty;
    const uint32_t dirty_since_before = s_persistence_dirty_since_ms;
    const esp_err_t ret = persist_store();
    if (ret != ESP_OK) {
        restore_blob(before);
        restore_rollback_authority();
        (void)__atomic_sub_fetch(
            &s_persistence_revision, 1U, __ATOMIC_RELEASE);
        s_persistence_dirty = dirty_before;
        s_persistence_dirty_since_ms = dirty_since_before;
        s_persistence_fail_count++;
        s_persistence_last_error = ret;
    } else if (!s_last_persist_durable) {
        mark_deferred_persistence_locked(
            (uint32_t)(esp_timer_get_time() / 1000ULL));
        s_persistence_last_error = ESP_OK;
    } else {
        if (s_last_persist_wrote) {
            s_persistence_commit_count++;
        }
        s_persistence_last_error = ESP_OK;
        s_persistence_dirty = false;
        s_persistence_dirty_since_ms = 0U;
        release_durable_mutation_authority_locked();
    }
    return ret;
}

static bool path_bytes_are_zero(const uint8_t *path, size_t offset)
{
    if (!path || offset > D1L_CONTACT_OUT_PATH_MAX) {
        return false;
    }
    for (size_t i = offset; i < D1L_CONTACT_OUT_PATH_MAX; ++i) {
        if (path[i] != 0U) {
            return false;
        }
    }
    return true;
}

static bool retained_path_record_is_valid(const d1l_contact_entry_t *entry)
{
    if (!entry ||
        !d1l_meshcore_path_lifecycle_valid(entry->out_path_state.lifecycle)) {
        return false;
    }
    switch ((d1l_meshcore_path_lifecycle_t)entry->out_path_state.lifecycle) {
    case D1L_MESHCORE_PATH_STATE_NONE: {
        const d1l_meshcore_path_state_t empty = {0};
        return !entry->out_path_valid && entry->out_path_len == 0U &&
               path_bytes_are_zero(entry->out_path, 0U) &&
               memcmp(&entry->out_path_state, &empty, sizeof(empty)) == 0;
    }
    case D1L_MESHCORE_PATH_STATE_VALID:
        return entry->out_path_valid &&
               entry->out_path_state.generation != 0U &&
                (entry->out_path_state.flags &
                 (uint8_t)~D1L_MESHCORE_PATH_FLAGS_MASK) == 0U &&
               entry->out_path_state.consecutive_failures <
                   D1L_MESHCORE_DIRECT_PATH_FAILURE_THRESHOLD &&
               d1l_meshcore_path_source_valid(entry->out_path_state.source) &&
               d1l_meshcore_wire_path_len_valid(entry->out_path_len) &&
               path_bytes_are_zero(
                   entry->out_path,
                   d1l_meshcore_wire_path_byte_len(entry->out_path_len));
    case D1L_MESHCORE_PATH_STATE_EXPIRED:
        return !entry->out_path_valid && entry->out_path_len == 0U &&
               path_bytes_are_zero(entry->out_path, 0U) &&
               entry->out_path_state.generation != 0U &&
                (entry->out_path_state.flags &
                 (uint8_t)~D1L_MESHCORE_PATH_FLAGS_MASK) == 0U &&
               d1l_meshcore_path_source_valid(entry->out_path_state.source);
    case D1L_MESHCORE_PATH_STATE_FAILED:
        return !entry->out_path_valid && entry->out_path_len == 0U &&
               path_bytes_are_zero(entry->out_path, 0U) &&
               entry->out_path_state.generation != 0U &&
                (entry->out_path_state.flags &
                 (uint8_t)~D1L_MESHCORE_PATH_FLAGS_MASK) == 0U &&
               entry->out_path_state.consecutive_failures >=
                   D1L_MESHCORE_DIRECT_PATH_FAILURE_THRESHOLD &&
               d1l_meshcore_path_source_valid(entry->out_path_state.source);
    default:
        return false;
    }
}

static bool blob_is_valid(const d1l_contact_store_blob_t *blob, size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_CONTACT_STORE_SCHEMA ||
        blob->count > D1L_CONTACT_STORE_CAPACITY || blob->next_seq == 0U) {
        return false;
    }
    for (size_t i = 0U; i < blob->count; ++i) {
        if (!retained_path_record_is_valid(&blob->entries[i])) {
            return false;
        }
    }
    return true;
}

static bool blob_v1_is_valid(const d1l_contact_store_blob_v1_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_CONTACT_STORE_SCHEMA_V1 &&
           blob->count <= D1L_CONTACT_STORE_LEGACY_CAPACITY &&
           blob->next_seq > 0;
}

static bool blob_v2_is_valid(const d1l_contact_store_blob_v2_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_CONTACT_STORE_SCHEMA_V2 &&
           blob->count <= D1L_CONTACT_STORE_LEGACY_CAPACITY &&
           blob->next_seq > 0;
}

static bool blob_v3_is_valid(const d1l_contact_store_blob_v3_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_CONTACT_STORE_SCHEMA_V3 &&
           blob->count <= D1L_CONTACT_STORE_LEGACY_CAPACITY &&
           blob->next_seq > 0;
}

static bool blob_v4_is_valid(const d1l_contact_store_blob_v4_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_CONTACT_STORE_SCHEMA_V4 &&
           blob->count <= D1L_CONTACT_STORE_LEGACY_CAPACITY &&
           blob->next_seq > 0;
}

static bool blob_v5_is_valid(const d1l_contact_store_blob_v5_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_CONTACT_STORE_SCHEMA_V5 &&
           blob->count <= D1L_CONTACT_STORE_LEGACY_CAPACITY &&
           blob->next_seq > 0;
}

static bool blob_v6_is_valid(const d1l_contact_store_blob_v6_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_CONTACT_STORE_SCHEMA_V6 &&
           blob->count <= D1L_CONTACT_STORE_LEGACY_CAPACITY &&
           blob->next_seq > 0;
}

static bool blob_v7_is_valid(const d1l_contact_store_blob_v7_t *blob,
                             size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_CONTACT_STORE_SCHEMA_V7 ||
        blob->count > D1L_CONTACT_STORE_LEGACY_CAPACITY ||
        blob->next_seq == 0U) {
        return false;
    }
    for (size_t i = 0U; i < blob->count; ++i) {
        if (!retained_path_record_is_valid(&blob->entries[i])) {
            return false;
        }
    }
    return true;
}

static void migrate_v1_blob(const d1l_contact_store_blob_v1_t *old_blob)
{
    clear_ram();
    s_count = old_blob->count;
    s_next_seq = old_blob->next_seq;
    s_total_written = old_blob->total_written;
    s_dropped_oldest = old_blob->dropped_oldest;
    for (size_t i = 0; i < s_count; ++i) {
        s_entries[i].seq = old_blob->entries[i].seq;
        s_entries[i].created_ms = old_blob->entries[i].created_ms;
        s_entries[i].updated_ms = old_blob->entries[i].updated_ms;
        memcpy(s_entries[i].fingerprint, old_blob->entries[i].fingerprint,
               sizeof(s_entries[i].fingerprint));
        memcpy(s_entries[i].alias, old_blob->entries[i].alias,
               sizeof(old_blob->entries[i].alias));
        memcpy(s_entries[i].heard_name, old_blob->entries[i].heard_name,
               sizeof(s_entries[i].heard_name));
        migrate_legacy_advert_type(s_entries[i].type, old_blob->entries[i].type);
        s_entries[i].last_rssi_dbm = old_blob->entries[i].last_rssi_dbm;
        s_entries[i].last_snr_tenths = old_blob->entries[i].last_snr_tenths;
        s_entries[i].path_hash_bytes = old_blob->entries[i].path_hash_bytes;
        s_entries[i].path_hops = old_blob->entries[i].path_hops;
        s_entries[i].favorite = old_blob->entries[i].favorite;
        s_entries[i].muted = old_blob->entries[i].muted;
        mark_migrated_verification(&s_entries[i]);
    }
}

static void migrate_v2_blob(const d1l_contact_store_blob_v2_t *old_blob)
{
    clear_ram();
    s_count = old_blob->count;
    s_next_seq = old_blob->next_seq;
    s_total_written = old_blob->total_written;
    s_dropped_oldest = old_blob->dropped_oldest;
    for (size_t i = 0; i < s_count; ++i) {
        s_entries[i].seq = old_blob->entries[i].seq;
        s_entries[i].created_ms = old_blob->entries[i].created_ms;
        s_entries[i].updated_ms = old_blob->entries[i].updated_ms;
        memcpy(s_entries[i].fingerprint, old_blob->entries[i].fingerprint,
               sizeof(s_entries[i].fingerprint));
        memcpy(s_entries[i].public_key_hex, old_blob->entries[i].public_key_hex,
               sizeof(s_entries[i].public_key_hex));
        memcpy(s_entries[i].alias, old_blob->entries[i].alias,
               sizeof(old_blob->entries[i].alias));
        memcpy(s_entries[i].heard_name, old_blob->entries[i].heard_name,
               sizeof(s_entries[i].heard_name));
        migrate_legacy_advert_type(s_entries[i].type, old_blob->entries[i].type);
        s_entries[i].last_rssi_dbm = old_blob->entries[i].last_rssi_dbm;
        s_entries[i].last_snr_tenths = old_blob->entries[i].last_snr_tenths;
        s_entries[i].path_hash_bytes = old_blob->entries[i].path_hash_bytes;
        s_entries[i].path_hops = old_blob->entries[i].path_hops;
        s_entries[i].favorite = old_blob->entries[i].favorite;
        s_entries[i].muted = old_blob->entries[i].muted;
        mark_migrated_verification(&s_entries[i]);
    }
}

static void migrate_v3_blob(const d1l_contact_store_blob_v3_t *old_blob)
{
    clear_ram();
    s_count = old_blob->count;
    s_next_seq = old_blob->next_seq;
    s_total_written = old_blob->total_written;
    s_dropped_oldest = old_blob->dropped_oldest;
    for (size_t i = 0; i < s_count; ++i) {
        s_entries[i].seq = old_blob->entries[i].seq;
        s_entries[i].created_ms = old_blob->entries[i].created_ms;
        s_entries[i].updated_ms = old_blob->entries[i].updated_ms;
        s_entries[i].out_path_updated_ms = old_blob->entries[i].out_path_updated_ms;
        memcpy(s_entries[i].fingerprint, old_blob->entries[i].fingerprint,
               sizeof(s_entries[i].fingerprint));
        memcpy(s_entries[i].public_key_hex, old_blob->entries[i].public_key_hex,
               sizeof(s_entries[i].public_key_hex));
        memcpy(s_entries[i].alias, old_blob->entries[i].alias,
               sizeof(old_blob->entries[i].alias));
        memcpy(s_entries[i].heard_name, old_blob->entries[i].heard_name,
               sizeof(s_entries[i].heard_name));
        migrate_legacy_advert_type(s_entries[i].type, old_blob->entries[i].type);
        s_entries[i].last_rssi_dbm = old_blob->entries[i].last_rssi_dbm;
        s_entries[i].last_snr_tenths = old_blob->entries[i].last_snr_tenths;
        s_entries[i].path_hash_bytes = old_blob->entries[i].path_hash_bytes;
        s_entries[i].path_hops = old_blob->entries[i].path_hops;
        s_entries[i].out_path_valid = old_blob->entries[i].out_path_valid;
        s_entries[i].out_path_len = old_blob->entries[i].out_path_len;
        memcpy(s_entries[i].out_path, old_blob->entries[i].out_path,
               sizeof(s_entries[i].out_path));
        s_entries[i].favorite = old_blob->entries[i].favorite;
        s_entries[i].muted = old_blob->entries[i].muted;
        mark_migrated_verification(&s_entries[i]);
    }
}

static void migrate_v4_blob(const d1l_contact_store_blob_v4_t *old_blob)
{
    clear_ram();
    s_count = old_blob->count;
    s_next_seq = old_blob->next_seq;
    s_total_written = old_blob->total_written;
    s_dropped_oldest = old_blob->dropped_oldest;
    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_contact_entry_v4_t *old = &old_blob->entries[i];
        d1l_contact_entry_t *entry = &s_entries[i];
        entry->seq = old->seq;
        entry->created_ms = old->created_ms;
        entry->updated_ms = old->updated_ms;
        entry->out_path_updated_ms = old->out_path_updated_ms;
        memcpy(entry->fingerprint, old->fingerprint, sizeof(entry->fingerprint));
        memcpy(entry->public_key_hex, old->public_key_hex,
               sizeof(entry->public_key_hex));
        memcpy(entry->alias, old->alias, sizeof(old->alias));
        memcpy(entry->heard_name, old->heard_name, sizeof(entry->heard_name));
        memcpy(entry->type, old->type, sizeof(entry->type));
        entry->last_rssi_dbm = old->last_rssi_dbm;
        entry->last_snr_tenths = old->last_snr_tenths;
        entry->path_hash_bytes = old->path_hash_bytes;
        entry->path_hops = old->path_hops;
        entry->out_path_valid = old->out_path_valid;
        entry->out_path_len = old->out_path_len;
        memcpy(entry->out_path, old->out_path, sizeof(entry->out_path));
        entry->favorite = old->favorite;
        entry->muted = old->muted;
        mark_migrated_verification(entry);
    }
}

static void migrate_v5_blob(const d1l_contact_store_blob_v5_t *old_blob)
{
    clear_ram();
    s_count = old_blob->count;
    s_next_seq = old_blob->next_seq;
    s_total_written = old_blob->total_written;
    s_dropped_oldest = old_blob->dropped_oldest;
    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_contact_entry_v5_t *old = &old_blob->entries[i];
        d1l_contact_entry_t *entry = &s_entries[i];
        entry->seq = old->seq;
        entry->created_ms = old->created_ms;
        entry->updated_ms = old->updated_ms;
        entry->out_path_updated_ms = old->out_path_updated_ms;
        memcpy(entry->fingerprint, old->fingerprint, sizeof(old->fingerprint));
        memcpy(entry->public_key_hex, old->public_key_hex,
               sizeof(old->public_key_hex));
        memcpy(entry->alias, old->alias, sizeof(old->alias));
        memcpy(entry->heard_name, old->heard_name, sizeof(old->heard_name));
        memcpy(entry->type, old->type, sizeof(old->type));
        entry->last_rssi_dbm = old->last_rssi_dbm;
        entry->last_snr_tenths = old->last_snr_tenths;
        entry->path_hash_bytes = old->path_hash_bytes;
        entry->path_hops = old->path_hops;
        entry->out_path_valid = old->out_path_valid;
        entry->out_path_len = old->out_path_len;
        memcpy(entry->out_path, old->out_path, sizeof(old->out_path));
        entry->favorite = old->favorite;
        entry->muted = old->muted;
        entry->verification_source = old->verification_source;
        entry->verified_at_ms = old->verified_at_ms;
        entry->signed_advert_timestamp = old->signed_advert_timestamp;
        entry->last_heard_ms = old->last_heard_ms;
    }
}

static void migrate_v6_blob(const d1l_contact_store_blob_v6_t *old_blob)
{
    clear_ram();
    s_count = old_blob->count;
    s_next_seq = old_blob->next_seq;
    s_total_written = old_blob->total_written;
    s_dropped_oldest = old_blob->dropped_oldest;
    for (size_t i = 0U; i < s_count; ++i) {
        memcpy(&s_entries[i], &old_blob->entries[i],
               sizeof(old_blob->entries[i]));
    }
}

static void migrate_v7_blob(const d1l_contact_store_blob_v7_t *old_blob)
{
    clear_ram();
    s_count = old_blob->count;
    s_next_seq = old_blob->next_seq;
    s_total_written = old_blob->total_written;
    s_dropped_oldest = old_blob->dropped_oldest;
    memcpy(s_entries, old_blob->entries,
           s_count * sizeof(old_blob->entries[0]));
}

static void finalize_migrated_path_state(void)
{
    for (size_t i = 0U; i < s_count; ++i) {
        d1l_contact_entry_t *entry = &s_entries[i];
        if (entry->out_path_valid &&
            d1l_meshcore_wire_path_len_valid(entry->out_path_len)) {
            const uint8_t path_bytes =
                d1l_meshcore_wire_path_byte_len(entry->out_path_len);
            memset(&entry->out_path[path_bytes], 0,
                   sizeof(entry->out_path) - path_bytes);
            (void)d1l_meshcore_path_state_learn(
                &entry->out_path_state,
                D1L_MESHCORE_PATH_SOURCE_MIGRATED,
                entry->out_path_updated_ms);
            continue;
        }
        entry->out_path_valid = false;
        entry->out_path_len = 0U;
        memset(entry->out_path, 0, sizeof(entry->out_path));
        d1l_meshcore_path_state_reset(&entry->out_path_state);
    }
}

static int find_index_by_fingerprint(const char *fingerprint)
{
    if (!fixed_hex_string_valid(fingerprint,
                                D1L_NODE_FINGERPRINT_LEN - 1U)) {
        return -1;
    }
    bool ambiguous = false;
    const int index =
        find_unique_index_by_fingerprint_hex(fingerprint, &ambiguous);
    return ambiguous ? -1 : index;
}

static bool contact_path_len_valid(uint8_t path_len)
{
    return d1l_meshcore_wire_path_len_valid(path_len) &&
           d1l_meshcore_wire_path_byte_len(path_len) <=
               D1L_CONTACT_OUT_PATH_MAX;
}

static bool is_hex_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static char lower_hex_char(char c)
{
    return (c >= 'A' && c <= 'F') ? (char)(c + ('a' - 'A')) : c;
}

static bool fixed_hex_string_valid(const char *value, size_t hex_chars)
{
    if (!value) {
        return false;
    }
    for (size_t i = 0; i < hex_chars; ++i) {
        if (!is_hex_char(value[i])) {
            return false;
        }
    }
    return value[hex_chars] == '\0';
}

static bool fixed_hex_strings_equal(const char *left, const char *right,
                                    size_t hex_chars)
{
    if (!left || !right) {
        return false;
    }
    for (size_t i = 0; i < hex_chars; ++i) {
        if (lower_hex_char(left[i]) != lower_hex_char(right[i])) {
            return false;
        }
    }
    return true;
}

static void copy_lower_hex(char *dest, size_t dest_size, const char *src,
                           size_t hex_chars)
{
    if (!dest || !src || dest_size <= hex_chars) {
        return;
    }
    for (size_t i = 0; i < hex_chars; ++i) {
        dest[i] = lower_hex_char(src[i]);
    }
    dest[hex_chars] = '\0';
}

static void sanitize_ascii_bounded(char *dest, size_t dest_size, const char *src,
                                   size_t src_size)
{
    if (!dest || dest_size == 0U) {
        return;
    }
    size_t out = 0U;
    while (src && out + 1U < dest_size && out < src_size && src[out] != '\0') {
        unsigned char c = (unsigned char)src[out];
        if (c < 32U || c > 126U || c == '"' || c == '\\') {
            c = '_';
        }
        dest[out++] = (char)c;
    }
    dest[out] = '\0';
}

static bool copy_bounded_text(char *dest, size_t dest_size, const char *src,
                              size_t src_size)
{
    if (!dest || dest_size == 0U) {
        return false;
    }
    dest[0] = '\0';
    if (!src) {
        return false;
    }
    size_t out = 0U;
    while (out < src_size && out + 1U < dest_size && src[out] != '\0') {
        dest[out] = src[out];
        out++;
    }
    if (out >= src_size || src[out] != '\0') {
        dest[0] = '\0';
        return false;
    }
    dest[out] = '\0';
    return out > 0U;
}

static int find_unique_index_by_fingerprint_hex(const char *fingerprint,
                                                bool *out_ambiguous)
{
    int found = -1;
    bool ambiguous = false;
    for (size_t i = 0; i < s_count; ++i) {
        if (!fixed_hex_string_valid(s_entries[i].fingerprint,
                                    D1L_NODE_FINGERPRINT_LEN - 1U) ||
            !fixed_hex_strings_equal(s_entries[i].fingerprint, fingerprint,
                                     D1L_NODE_FINGERPRINT_LEN - 1U)) {
            continue;
        }
        if (found >= 0) {
            ambiguous = true;
            break;
        }
        found = (int)i;
    }
    if (out_ambiguous) {
        *out_ambiguous = ambiguous;
    }
    return found;
}

static int find_unique_index_by_public_key_hex(const char *public_key_hex,
                                               bool *out_ambiguous)
{
    int found = -1;
    bool ambiguous = false;
    for (size_t i = 0; i < s_count; ++i) {
        if (!fixed_hex_string_valid(s_entries[i].public_key_hex,
                                    D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U) ||
            !fixed_hex_strings_equal(s_entries[i].public_key_hex, public_key_hex,
                                     D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U)) {
            continue;
        }
        if (found >= 0) {
            ambiguous = true;
            break;
        }
        found = (int)i;
    }
    if (out_ambiguous) {
        *out_ambiguous = ambiguous;
    }
    return found;
}

static uint8_t contact_path_byte_len(uint8_t path_len)
{
    return d1l_meshcore_wire_path_byte_len(path_len);
}

static int oldest_evictable_placeholder_index(void)
{
    int oldest = -1;
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[i].favorite ||
            d1l_contact_store_is_canonical(&s_entries[i])) {
            continue;
        }
        if (oldest < 0 || s_entries[i].seq < s_entries[(size_t)oldest].seq) {
            oldest = (int)i;
        }
    }
    return oldest;
}

static const char *meshcore_type_name(uint8_t type_id)
{
    switch (type_id) {
    case 1U:
        return "chat";
    case 2U:
        return "repeater";
    case 3U:
        return "room";
    case 4U:
        return "sensor";
    default:
        return NULL;
    }
}

esp_err_t d1l_contact_store_init(void)
{
    d1l_retained_blob_store_backend_state_t backend = {0};
    if (!d1l_retained_blob_store_backend_state(
            D1L_RETAINED_BLOB_STORE_CONTACTS, &backend)) {
        return ESP_ERR_INVALID_STATE;
    }
    clear_ram();
    reset_persistence_state(backend.generation);
    s_loaded = false;
    bool migrated = false;
    bool migrated_path_layout = false;
    bool loaded_from_legacy = false;
    bool loaded_from_sd = false;

    memset(&s_blob_scratch, 0, sizeof(s_blob_scratch));
    size_t len = sizeof(s_blob_scratch);
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (backend.enabled) {
        ret = d1l_retained_blob_store_read_sd_primary(
            D1L_RETAINED_BLOB_STORE_CONTACTS, D1L_CONTACT_STORE_KEY,
            &s_blob_scratch, &len);
        loaded_from_sd = ret == ESP_OK;
    }
    if (!backend.enabled || ret == ESP_ERR_NOT_FOUND) {
        loaded_from_sd = false;
        memset(&s_blob_scratch, 0, sizeof(s_blob_scratch));
        len = sizeof(s_blob_scratch);
        ret = d1l_retained_blob_store_read(
            D1L_RETAINED_BLOB_STORE_CONTACTS,
            D1L_CONTACT_STORE_KEY, &s_blob_scratch, &len);
        loaded_from_legacy = ret == ESP_OK;
    }
    if (ret == ESP_ERR_NOT_FOUND) {
        ret = ESP_OK;
    } else if (ret == ESP_OK && blob_is_valid(&s_blob_scratch, len)) {
        memcpy(s_entries, s_blob_scratch.entries, sizeof(s_entries));
        s_count = s_blob_scratch.count;
        s_next_seq = s_blob_scratch.next_seq;
        s_total_written = s_blob_scratch.total_written;
        s_dropped_oldest = s_blob_scratch.dropped_oldest;
    } else if (ret == ESP_OK && blob_v7_is_valid(
                   (const d1l_contact_store_blob_v7_t *)&s_blob_scratch,
                   len)) {
        migrate_v7_blob(
            (const d1l_contact_store_blob_v7_t *)&s_blob_scratch);
        migrated = true;
    } else if (ret == ESP_OK &&
               blob_v6_is_valid((const d1l_contact_store_blob_v6_t *)&s_blob_scratch, len)) {
        migrate_v6_blob((const d1l_contact_store_blob_v6_t *)&s_blob_scratch);
        migrated = true;
        migrated_path_layout = true;
    } else if (ret == ESP_OK &&
               blob_v5_is_valid((const d1l_contact_store_blob_v5_t *)&s_blob_scratch, len)) {
        migrate_v5_blob((const d1l_contact_store_blob_v5_t *)&s_blob_scratch);
        migrated = true;
        migrated_path_layout = true;
    } else if (ret == ESP_OK &&
               blob_v4_is_valid((const d1l_contact_store_blob_v4_t *)&s_blob_scratch, len)) {
        migrate_v4_blob((const d1l_contact_store_blob_v4_t *)&s_blob_scratch);
        migrated = true;
        migrated_path_layout = true;
    } else if (ret == ESP_OK &&
               blob_v3_is_valid((const d1l_contact_store_blob_v3_t *)&s_blob_scratch, len)) {
        migrate_v3_blob((const d1l_contact_store_blob_v3_t *)&s_blob_scratch);
        migrated = true;
        migrated_path_layout = true;
    } else if (ret == ESP_OK &&
               blob_v1_is_valid((const d1l_contact_store_blob_v1_t *)&s_blob_scratch, len)) {
        migrate_v1_blob((const d1l_contact_store_blob_v1_t *)&s_blob_scratch);
        migrated = true;
        migrated_path_layout = true;
    } else if (ret == ESP_OK &&
               blob_v2_is_valid((const d1l_contact_store_blob_v2_t *)&s_blob_scratch, len)) {
        migrate_v2_blob((const d1l_contact_store_blob_v2_t *)&s_blob_scratch);
        migrated = true;
        migrated_path_layout = true;
    } else if (ret == ESP_OK) {
        /* A retained contact blob is user data.  An unrecognised schema or a
         * corrupt record must never be converted into an empty, committed
         * store during boot.  Keep the original retained value byte-for-byte,
         * expose no contacts, and make every implicit re-open fail closed
         * until the user explicitly clears the store. */
        clear_ram();
        ret = s_blob_scratch.schema > D1L_CONTACT_STORE_SCHEMA ?
                  ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK && migrated_path_layout) {
        finalize_migrated_path_state();
    }
    d1l_retained_blob_store_backend_state_t settled_backend = {0};
    if (ret == ESP_OK &&
        (!d1l_retained_blob_store_backend_state(
             D1L_RETAINED_BLOB_STORE_CONTACTS, &settled_backend) ||
         settled_backend.enabled != backend.enabled ||
         settled_backend.generation != backend.generation)) {
        clear_ram();
        s_sd_reconcile_pending = true;
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK && (migrated || loaded_from_legacy)) {
        s_mutation_authority = D1L_CONTACT_MUTATION_AUTHORITY_LOCAL;
        if (loaded_from_legacy) {
            remember_all_current_fingerprints_locked();
        }
        mark_deferred_persistence_locked(
            (uint32_t)(esp_timer_get_time() / 1000ULL));
        /* A validated legacy schema read from this exact mounted generation
         * can be upgraded directly under the guarded write. NVS fallback data
         * must still reconcile against the currently mounted SD first. */
        s_sd_reconcile_pending = backend.enabled && !loaded_from_sd;
        ret = persist_store();
        if (ret == ESP_OK && s_last_persist_durable) {
            s_persistence_dirty = false;
            s_persistence_dirty_since_ms = 0U;
            release_durable_mutation_authority_locked();
        }
    }
    s_loaded = (ret == ESP_OK);
    return ret;
}

static esp_err_t flush_deferred_path_state(bool force)
{
    if (!s_loaded) {
        const esp_err_t init_ret = d1l_contact_store_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }

    d1l_store_lock_take(&s_deferred_flush_lock);
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    d1l_retained_blob_store_backend_state_t backend = {0};
    if (!d1l_retained_blob_store_backend_state(
            D1L_RETAINED_BLOB_STORE_CONTACTS, &backend)) {
        d1l_store_lock_give(&s_deferred_flush_lock);
        return ESP_ERR_INVALID_STATE;
    }
    d1l_store_lock_take(&s_store_lock);
    const bool backend_generation_changed =
        backend.enabled &&
        backend.generation != s_accepted_sd_backend_generation;
    if (!s_persistence_dirty && !s_sd_reconcile_pending &&
        !backend_generation_changed) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_deferred_flush_lock);
        return ESP_OK;
    }
    if (!backend.enabled) {
        s_sd_reconcile_pending = true;
        if (s_persistence_dirty) {
            s_persistence_coalesced_count++;
        }
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_deferred_flush_lock);
        return ESP_OK;
    }
    if (backend_generation_changed || s_sd_reconcile_pending) {
        const esp_err_t ret = persist_store();
        if (ret != ESP_OK) {
            s_persistence_fail_count++;
            s_persistence_last_error = ret;
        } else if (!s_last_persist_durable) {
            mark_deferred_persistence_locked(now_ms);
        } else {
            if (s_last_persist_wrote) {
                s_persistence_commit_count++;
            }
            s_persistence_last_error = ESP_OK;
            s_persistence_dirty = false;
            s_persistence_dirty_since_ms = 0U;
            release_durable_mutation_authority_locked();
        }
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_deferred_flush_lock);
        return ret;
    }
    if (!force &&
        (uint32_t)(now_ms - s_persistence_dirty_since_ms) <
            D1L_CONTACT_PATH_PERSIST_MIN_INTERVAL_MS) {
        s_persistence_coalesced_count++;
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_deferred_flush_lock);
        return ESP_OK;
    }
    fill_blob(&s_persist_snapshot);
    const uint32_t snapshot_revision = __atomic_load_n(
        &s_persistence_revision, __ATOMIC_ACQUIRE);
    const bool erase_snapshot =
        s_mutation_authority ==
            D1L_CONTACT_MUTATION_AUTHORITY_EXPLICIT_CLEAR &&
        s_count == 0U;
    d1l_store_lock_give(&s_store_lock);

    d1l_store_lock_take(&s_persist_io_lock);
    if (__atomic_load_n(&s_persistence_revision, __ATOMIC_ACQUIRE) !=
        snapshot_revision) {
        d1l_store_lock_give(&s_persist_io_lock);
        d1l_store_lock_take(&s_store_lock);
        s_persistence_coalesced_count++;
        if (force) {
            s_persistence_fail_count++;
            s_persistence_last_error = ESP_ERR_INVALID_STATE;
        }
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_deferred_flush_lock);
        return force ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    esp_err_t ret = contact_sd_backend_generation_matches(
                        backend.generation) ?
        ESP_OK : ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK) {
        ret = erase_snapshot ?
            d1l_retained_blob_store_erase_sd_primary_guarded(
                D1L_RETAINED_BLOB_STORE_CONTACTS, D1L_CONTACT_STORE_KEY,
                backend.generation) :
            d1l_retained_blob_store_write_sd_primary_guarded(
                D1L_RETAINED_BLOB_STORE_CONTACTS, D1L_CONTACT_STORE_KEY,
                &s_persist_snapshot, sizeof(s_persist_snapshot),
                backend.generation);
    }
    if (ret == ESP_OK &&
        !contact_sd_backend_generation_matches(backend.generation)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        ret = d1l_retained_blob_store_erase_nvs_fallback(
            D1L_RETAINED_BLOB_STORE_CONTACTS, D1L_CONTACT_STORE_KEY);
    }
    d1l_store_lock_give(&s_persist_io_lock);

    d1l_store_lock_take(&s_store_lock);
    esp_err_t result = ret;
    if (ret != ESP_OK) {
        s_persistence_fail_count++;
        s_persistence_last_error = ret;
        if (ret == ESP_ERR_INVALID_STATE) {
            s_sd_reconcile_pending = true;
        }
    } else {
        s_persistence_commit_count++;
        s_persistence_last_error = ESP_OK;
        if (__atomic_load_n(&s_persistence_revision, __ATOMIC_ACQUIRE) ==
            snapshot_revision) {
            s_persistence_dirty = false;
            s_persistence_dirty_since_ms = 0U;
            release_durable_mutation_authority_locked();
        } else {
            s_persistence_coalesced_count++;
            if (force) {
                s_persistence_fail_count++;
                s_persistence_last_error = ESP_ERR_INVALID_STATE;
                result = ESP_ERR_INVALID_STATE;
            }
        }
    }
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_deferred_flush_lock);
    return result;
}

esp_err_t d1l_contact_store_flush(void)
{
    return flush_deferred_path_state(true);
}

esp_err_t d1l_contact_store_flush_if_due(void)
{
    return flush_deferred_path_state(false);
}

esp_err_t d1l_contact_store_clear(void)
{
    if (!s_loaded) {
        const esp_err_t init_ret = d1l_contact_store_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }
    d1l_store_lock_take(&s_store_lock);
    const esp_err_t revision_ret =
        reserve_persistence_revision_locked(false);
    if (revision_ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        return revision_ret;
    }
    clear_ram();
    s_mutation_authority =
        D1L_CONTACT_MUTATION_AUTHORITY_EXPLICIT_CLEAR;
    s_deleted_fingerprint_count = 0U;
    memset(s_deleted_fingerprints, 0, sizeof(s_deleted_fingerprints));
    s_touched_fingerprint_count = 0U;
    memset(s_touched_fingerprints, 0, sizeof(s_touched_fingerprints));
    s_loaded = true;
    mark_deferred_persistence_locked(
        (uint32_t)(esp_timer_get_time() / 1000ULL));

    const esp_err_t ret = persist_store();
    if (ret == ESP_OK && s_last_persist_durable) {
        if (s_last_persist_wrote) {
            s_persistence_commit_count++;
        }
        s_persistence_last_error = ESP_OK;
        s_persistence_dirty = false;
        s_persistence_dirty_since_ms = 0U;
        release_durable_mutation_authority_locked();
    } else if (ret != ESP_OK) {
        s_persistence_fail_count++;
        s_persistence_last_error = ret;
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

esp_err_t d1l_contact_store_upsert_from_node(const char *fingerprint, const char *alias,
                                             const d1l_node_entry_t *heard_node)
{
    if (!fixed_hex_string_valid(fingerprint,
                                D1L_NODE_FINGERPRINT_LEN - 1U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        esp_err_t ret = d1l_contact_store_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    bool fingerprint_ambiguous = false;
    int existing = find_unique_index_by_fingerprint_hex(
        fingerprint, &fingerprint_ambiguous);
    if (fingerprint_ambiguous) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_STATE;
    }
    size_t index;
    const bool is_new = existing < 0;
    bool append_new = false;
    bool evict_placeholder = false;
    if (!is_new) {
        index = (size_t)existing;
    } else if (s_count < D1L_CONTACT_STORE_CAPACITY) {
        index = s_count;
        append_new = true;
    } else {
        const int evictable = oldest_evictable_placeholder_index();
        if (evictable < 0) {
            d1l_store_lock_give(&s_store_lock);
            return ESP_ERR_NO_MEM;
        }
        index = (size_t)evictable;
        evict_placeholder = true;
    }
    capture_rollback_state();
    esp_err_t ret = reserve_sequenced_mutation_locked();
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        return ret;
    }
    if (evict_placeholder) {
        remember_deleted_fingerprint_locked(s_entries[index].fingerprint);
    }
    remember_touched_fingerprint_locked(fingerprint);
    if (append_new) {
        s_count++;
    } else if (evict_placeholder) {
        s_dropped_oldest++;
    }

    d1l_contact_entry_t *entry = &s_entries[index];
    const bool canonical_before = !is_new &&
                                  d1l_contact_store_is_canonical(entry);
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (is_new) {
        memset(entry, 0, sizeof(*entry));
        entry->created_ms = now_ms;
    }
    entry->seq = s_next_seq++;
    entry->updated_ms = now_ms;
    copy_lower_hex(entry->fingerprint, sizeof(entry->fingerprint), fingerprint,
                   D1L_NODE_FINGERPRINT_LEN - 1U);

    if (heard_node) {
        /* A passive observation is not identity authorization. Full keys and
         * canonical roles enter only through signed-advert or URI import. */
        if (!canonical_before && heard_node->name[0] != '\0') {
            sanitize_ascii(entry->heard_name, sizeof(entry->heard_name), heard_node->name);
        }
        if (!canonical_before && heard_node->type[0] != '\0') {
            sanitize_ascii(entry->type, sizeof(entry->type), heard_node->type);
        }
        entry->last_rssi_dbm = heard_node->rssi_dbm;
        entry->last_snr_tenths = heard_node->snr_tenths;
        entry->path_hash_bytes = heard_node->path_hash_bytes;
        entry->path_hops = heard_node->path_hops;
    }

    if (alias && alias[0] != '\0') {
        sanitize_ascii(entry->alias, sizeof(entry->alias), alias);
    } else if (is_new || entry->alias[0] == '\0') {
        if (heard_node && heard_node->name[0] != '\0') {
            sanitize_ascii(entry->alias, sizeof(entry->alias), heard_node->name);
        } else {
            sanitize_ascii(entry->alias, sizeof(entry->alias), fingerprint);
        }
    }

    if (entry->heard_name[0] == '\0') {
        sanitize_ascii(entry->heard_name, sizeof(entry->heard_name), entry->alias);
    }
    if (entry->type[0] == '\0') {
        sanitize_ascii(entry->type, sizeof(entry->type), "unknown");
    }

    ret = persist_store_or_rollback(&s_rollback_scratch);
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

esp_err_t d1l_contact_store_upsert_verified_advert(
    const char *fingerprint, const d1l_node_entry_t *verified_node,
    d1l_contact_verified_advert_result_t *out_result,
    d1l_contact_entry_t *out_entry)
{
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = D1L_CONTACT_VERIFIED_ADVERT_NONE;
    if (out_entry) {
        memset(out_entry, 0, sizeof(*out_entry));
    }
    if (!verified_node ||
        !fixed_hex_string_valid(fingerprint, D1L_NODE_FINGERPRINT_LEN - 1U) ||
        !fixed_hex_string_valid(verified_node->public_key_hex,
                                D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U) ||
        !fixed_hex_strings_equal(fingerprint, verified_node->public_key_hex,
                                 D1L_NODE_FINGERPRINT_LEN - 1U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        esp_err_t ret = d1l_contact_store_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    bool fingerprint_ambiguous = false;
    bool public_key_ambiguous = false;
    const int fingerprint_index = find_unique_index_by_fingerprint_hex(
        fingerprint, &fingerprint_ambiguous);
    const int public_key_index = find_unique_index_by_public_key_hex(
        verified_node->public_key_hex, &public_key_ambiguous);
    if (fingerprint_ambiguous || public_key_ambiguous ||
        (public_key_index >= 0 && fingerprint_index != public_key_index)) {
        *out_result = D1L_CONTACT_VERIFIED_ADVERT_COLLISION;
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_STATE;
    }

    size_t index = 0U;
    d1l_contact_verified_advert_result_t result =
        D1L_CONTACT_VERIFIED_ADVERT_NONE;
    if (public_key_index >= 0) {
        index = (size_t)public_key_index;
        result = D1L_CONTACT_VERIFIED_ADVERT_UPDATED;
    } else if (fingerprint_index >= 0) {
        index = (size_t)fingerprint_index;
        if (s_entries[index].public_key_hex[0] != '\0') {
            *out_result = D1L_CONTACT_VERIFIED_ADVERT_COLLISION;
            d1l_store_lock_give(&s_store_lock);
            return ESP_ERR_INVALID_STATE;
        }
        result = D1L_CONTACT_VERIFIED_ADVERT_PROMOTED_PLACEHOLDER;
    } else if (s_count >= D1L_CONTACT_STORE_CAPACITY) {
        *out_result = D1L_CONTACT_VERIFIED_ADVERT_FULL;
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_NO_MEM;
    } else {
        index = s_count;
        result = D1L_CONTACT_VERIFIED_ADVERT_CREATED;
    }

    const esp_err_t revision_ret = reserve_sequenced_mutation_locked();
    if (revision_ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        return revision_ret;
    }
    remember_touched_fingerprint_locked(fingerprint);
    if (result == D1L_CONTACT_VERIFIED_ADVERT_CREATED) {
        s_count++;
    }
    d1l_contact_entry_t *entry = &s_entries[index];
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (result == D1L_CONTACT_VERIFIED_ADVERT_CREATED) {
        memset(entry, 0, sizeof(*entry));
        entry->created_ms = now_ms;
    }
    const bool alias_tracks_advert =
        result != D1L_CONTACT_VERIFIED_ADVERT_PROMOTED_PLACEHOLDER &&
        (entry->alias[0] == '\0' ||
         strcmp(entry->alias, entry->heard_name) == 0);
    entry->seq = s_next_seq++;
    entry->updated_ms = now_ms;
    copy_lower_hex(entry->fingerprint, sizeof(entry->fingerprint), fingerprint,
                   D1L_NODE_FINGERPRINT_LEN - 1U);
    copy_lower_hex(entry->public_key_hex, sizeof(entry->public_key_hex),
                   verified_node->public_key_hex,
                   D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U);
    if (verified_node->name[0] != '\0') {
        sanitize_ascii_bounded(entry->heard_name, sizeof(entry->heard_name),
                               verified_node->name, sizeof(verified_node->name));
        if (alias_tracks_advert) {
            sanitize_ascii_bounded(entry->alias, sizeof(entry->alias),
                                   verified_node->name,
                                   sizeof(verified_node->name));
        }
    }
    if (verified_node->type[0] != '\0') {
        sanitize_ascii_bounded(entry->type, sizeof(entry->type),
                               verified_node->type, sizeof(verified_node->type));
    }
    entry->last_rssi_dbm = verified_node->rssi_dbm;
    entry->last_snr_tenths = verified_node->snr_tenths;
    entry->path_hash_bytes = verified_node->path_hash_bytes;
    entry->path_hops = verified_node->path_hops;
    entry->verification_source = D1L_CONTACT_VERIFICATION_SIGNED_ADVERT;
    entry->verified_at_ms = now_ms;
    entry->signed_advert_timestamp = verified_node->advert_timestamp;
    entry->last_heard_ms = verified_node->last_heard_ms;

    if (entry->alias[0] == '\0') {
        if (verified_node->name[0] != '\0') {
            sanitize_ascii_bounded(entry->alias, sizeof(entry->alias),
                                   verified_node->name, sizeof(verified_node->name));
        } else {
            copy_lower_hex(entry->alias, sizeof(entry->alias), fingerprint,
                           D1L_NODE_FINGERPRINT_LEN - 1U);
        }
    }
    if (entry->heard_name[0] == '\0') {
        sanitize_ascii(entry->heard_name, sizeof(entry->heard_name), entry->alias);
    }
    if (entry->type[0] == '\0') {
        sanitize_ascii(entry->type, sizeof(entry->type), "unknown");
    }

    /* Signed adverts are ambient traffic, so publish the verified identity to
     * readers immediately and let the retained-store worker coalesce the SD
     * write. Explicit user edits still use synchronous persistence. */
    mark_deferred_persistence_locked(now_ms);
    *out_result = result;
    if (out_entry) {
        *out_entry = s_entries[index];
    }
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

esp_err_t d1l_contact_store_import_uri(
    const char *uri, size_t uri_len, d1l_contact_import_result_t *out_result,
    d1l_contact_entry_t *out_entry)
{
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = D1L_CONTACT_IMPORT_NONE;
    if (out_entry) {
        memset(out_entry, 0, sizeof(*out_entry));
    }

    d1l_contact_uri_t imported = {0};
    if (!d1l_contact_uri_parse(uri, uri_len, &imported)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *imported_type = meshcore_type_name(imported.type_id);
    if (!imported_type) {
        return ESP_ERR_INVALID_ARG;
    }

    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    memcpy(fingerprint, imported.public_key_hex,
           D1L_NODE_FINGERPRINT_LEN - 1U);
    if (!s_loaded) {
        const esp_err_t init_ret = d1l_contact_store_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    bool fingerprint_ambiguous = false;
    bool public_key_ambiguous = false;
    const int fingerprint_index = find_unique_index_by_fingerprint_hex(
        fingerprint, &fingerprint_ambiguous);
    const int public_key_index = find_unique_index_by_public_key_hex(
        imported.public_key_hex, &public_key_ambiguous);
    if (fingerprint_ambiguous || public_key_ambiguous ||
        (public_key_index >= 0 && fingerprint_index != public_key_index)) {
        *out_result = D1L_CONTACT_IMPORT_COLLISION;
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_STATE;
    }

    size_t index = 0U;
    d1l_contact_import_result_t result = D1L_CONTACT_IMPORT_NONE;
    if (public_key_index >= 0) {
        index = (size_t)public_key_index;
        const uint8_t retained_type =
            d1l_contact_store_meshcore_type_id(s_entries[index].type);
        const bool signed_role_authoritative =
            s_entries[index].verification_source ==
                D1L_CONTACT_VERIFICATION_SIGNED_ADVERT ||
            s_entries[index].verification_source ==
                D1L_CONTACT_VERIFICATION_MIGRATED_SIGNED_ADVERT;
        if ((retained_type != 0U && retained_type != imported.type_id) ||
            (signed_role_authoritative && retained_type == 0U)) {
            *out_result = D1L_CONTACT_IMPORT_ROLE_CONFLICT;
            d1l_store_lock_give(&s_store_lock);
            return ESP_ERR_INVALID_STATE;
        }
        result = D1L_CONTACT_IMPORT_UPDATED;
    } else if (fingerprint_index >= 0) {
        index = (size_t)fingerprint_index;
        if (s_entries[index].public_key_hex[0] != '\0') {
            *out_result = D1L_CONTACT_IMPORT_COLLISION;
            d1l_store_lock_give(&s_store_lock);
            return ESP_ERR_INVALID_STATE;
        }
        result = D1L_CONTACT_IMPORT_PROMOTED_PLACEHOLDER;
    } else if (s_count >= D1L_CONTACT_STORE_CAPACITY) {
        *out_result = D1L_CONTACT_IMPORT_FULL;
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_NO_MEM;
    } else {
        index = s_count;
        result = D1L_CONTACT_IMPORT_CREATED;
    }

    if (result == D1L_CONTACT_IMPORT_UPDATED) {
        const d1l_contact_entry_t *retained = &s_entries[index];
        const bool source_is_authoritative =
            retained->verification_source ==
                D1L_CONTACT_VERIFICATION_SIGNED_ADVERT ||
            retained->verification_source ==
                D1L_CONTACT_VERIFICATION_URI_IMPORT ||
            retained->verification_source ==
                D1L_CONTACT_VERIFICATION_MIGRATED_SIGNED_ADVERT;
        if (source_is_authoritative && retained->alias[0] != '\0') {
            *out_result = result;
            if (out_entry) {
                *out_entry = *retained;
            }
            d1l_store_lock_give(&s_store_lock);
            return ESP_OK;
        }
    }

    capture_rollback_state();
    const esp_err_t revision_ret = reserve_sequenced_mutation_locked();
    if (revision_ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        return revision_ret;
    }
    remember_touched_fingerprint_locked(fingerprint);
    if (result == D1L_CONTACT_IMPORT_CREATED) {
        s_count++;
    }
    d1l_contact_entry_t *entry = &s_entries[index];
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (result == D1L_CONTACT_IMPORT_CREATED) {
        memset(entry, 0, sizeof(*entry));
        entry->created_ms = now_ms;
    }
    entry->seq = s_next_seq++;
    entry->updated_ms = now_ms;
    copy_lower_hex(entry->fingerprint, sizeof(entry->fingerprint), fingerprint,
                   D1L_NODE_FINGERPRINT_LEN - 1U);
    copy_lower_hex(entry->public_key_hex, sizeof(entry->public_key_hex),
                   imported.public_key_hex,
                   D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U);
    sanitize_ascii_bounded(entry->type, sizeof(entry->type), imported_type,
                           strlen(imported_type));
    if (entry->alias[0] == '\0') {
        memcpy(entry->alias, imported.name, sizeof(entry->alias));
    }
    if (entry->verification_source != D1L_CONTACT_VERIFICATION_SIGNED_ADVERT &&
        entry->verification_source !=
            D1L_CONTACT_VERIFICATION_MIGRATED_SIGNED_ADVERT) {
        entry->verification_source = D1L_CONTACT_VERIFICATION_URI_IMPORT;
        entry->verified_at_ms = now_ms;
        entry->signed_advert_timestamp = 0U;
        entry->last_heard_ms = 0U;
    }

    const esp_err_t ret = persist_store_or_rollback(&s_rollback_scratch);
    if (ret == ESP_OK) {
        *out_result = result;
        if (out_entry) {
            *out_entry = s_entries[index];
        }
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

esp_err_t d1l_contact_store_update_path(const char *fingerprint, const uint8_t *path,
                                        uint8_t path_len)
{
    return d1l_contact_store_update_path_from_source(
        fingerprint, path, path_len, D1L_MESHCORE_PATH_SOURCE_OBSERVED, NULL);
}

esp_err_t d1l_contact_store_update_path_from_source(
    const char *fingerprint, const uint8_t *path, uint8_t path_len,
    d1l_meshcore_path_source_t source, d1l_contact_entry_t *out_entry)
{
    if (!fingerprint || fingerprint[0] == '\0' ||
        !contact_path_len_valid(path_len) ||
        !d1l_meshcore_path_source_valid((uint8_t)source)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t bytes = contact_path_byte_len(path_len);
    if (bytes > 0 && path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        esp_err_t ret = d1l_contact_store_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    int existing = find_index_by_fingerprint(fingerprint);
    if (existing < 0) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_NOT_FOUND;
    }

    d1l_contact_entry_t *entry = &s_entries[(size_t)existing];
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    d1l_meshcore_path_state_t next_state = entry->out_path_state;
    if (!d1l_meshcore_path_state_learn(
            &next_state, source, now_ms)) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t revision_ret = reserve_sequenced_mutation_locked();
    if (revision_ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        return revision_ret;
    }
    remember_touched_fingerprint_locked(entry->fingerprint);
    entry->out_path_state = next_state;
    entry->seq = s_next_seq++;
    entry->updated_ms = now_ms;
    entry->out_path_updated_ms = now_ms;
    entry->out_path_valid = true;
    entry->out_path_len = path_len;
    memset(entry->out_path, 0, sizeof(entry->out_path));
    if (bytes > 0) {
        memcpy(entry->out_path, path, bytes);
    }
    mark_deferred_persistence_locked(now_ms);
    if (out_entry) {
        *out_entry = *entry;
    }
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

esp_err_t d1l_contact_store_reset_path(
    const char *fingerprint, d1l_contact_entry_t *out_entry)
{
    if (!fingerprint || fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        const esp_err_t init_ret = d1l_contact_store_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    const int existing = find_index_by_fingerprint(fingerprint);
    if (existing < 0) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_NOT_FOUND;
    }
    d1l_contact_entry_t *entry = &s_entries[(size_t)existing];
    const d1l_meshcore_path_state_t empty_state = {0};
    const bool already_clear =
        !entry->out_path_valid && entry->out_path_len == 0U &&
        memcmp(entry->out_path, (uint8_t[D1L_CONTACT_OUT_PATH_MAX]){0},
               sizeof(entry->out_path)) == 0 &&
        memcmp(&entry->out_path_state, &empty_state,
               sizeof(entry->out_path_state)) == 0;
    if (!already_clear) {
        const esp_err_t revision_ret = reserve_sequenced_mutation_locked();
        if (revision_ret != ESP_OK) {
            d1l_store_lock_give(&s_store_lock);
            return revision_ret;
        }
        remember_touched_fingerprint_locked(entry->fingerprint);
        const uint32_t now_ms =
            (uint32_t)(esp_timer_get_time() / 1000ULL);
        d1l_meshcore_path_state_reset(&entry->out_path_state);
        entry->seq = s_next_seq++;
        entry->updated_ms = now_ms;
        entry->out_path_updated_ms = now_ms;
        entry->out_path_valid = false;
        entry->out_path_len = 0U;
        memset(entry->out_path, 0, sizeof(entry->out_path));
        mark_deferred_persistence_locked(now_ms);
    }
    if (out_entry) {
        *out_entry = *entry;
    }
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

esp_err_t d1l_contact_store_prepare_path_route(
    const char *fingerprint, uint32_t now_ms, d1l_contact_entry_t *out_entry,
    bool *out_expired)
{
    if (!fingerprint || fingerprint[0] == '\0' || !out_entry || !out_expired) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_expired = false;
    if (!s_loaded) {
        const esp_err_t init_ret = d1l_contact_store_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    const int existing = find_index_by_fingerprint(fingerprint);
    if (existing < 0) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_NOT_FOUND;
    }
    d1l_contact_entry_t *entry = &s_entries[(size_t)existing];
    d1l_meshcore_path_state_t next_state = entry->out_path_state;
    if (!d1l_meshcore_path_state_expire_if_due(
            &next_state, now_ms)) {
        *out_entry = *entry;
        d1l_store_lock_give(&s_store_lock);
        return ESP_OK;
    }
    const esp_err_t revision_ret = reserve_sequenced_mutation_locked();
    if (revision_ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        return revision_ret;
    }
    remember_touched_fingerprint_locked(entry->fingerprint);

    entry->out_path_state = next_state;
    entry->seq = s_next_seq++;
    entry->updated_ms = now_ms;
    entry->out_path_valid = false;
    entry->out_path_len = 0U;
    memset(entry->out_path, 0, sizeof(entry->out_path));
    mark_deferred_persistence_locked(now_ms);
    *out_entry = *entry;
    *out_expired = true;
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

esp_err_t d1l_contact_store_note_path_result(
    const char *fingerprint, uint32_t expected_generation, bool success,
    uint32_t now_ms, d1l_contact_entry_t *out_entry,
    d1l_meshcore_path_result_t *out_result)
{
    if (!fingerprint || fingerprint[0] == '\0' || expected_generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_result) {
        *out_result = D1L_MESHCORE_PATH_RESULT_STALE;
    }
    if (!s_loaded) {
        const esp_err_t init_ret = d1l_contact_store_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    const int existing = find_index_by_fingerprint(fingerprint);
    if (existing < 0) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_NOT_FOUND;
    }
    d1l_contact_entry_t *entry = &s_entries[(size_t)existing];
    d1l_meshcore_path_state_t next_state = entry->out_path_state;
    const d1l_meshcore_path_result_t result =
        d1l_meshcore_path_state_note_direct_result(
            &next_state, expected_generation, success, now_ms);
    if (result == D1L_MESHCORE_PATH_RESULT_STALE) {
        if (out_entry) {
            *out_entry = *entry;
        }
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t revision_ret = reserve_sequenced_mutation_locked();
    if (revision_ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        return revision_ret;
    }
    remember_touched_fingerprint_locked(entry->fingerprint);

    entry->out_path_state = next_state;
    entry->seq = s_next_seq++;
    entry->updated_ms = now_ms;
    if (result == D1L_MESHCORE_PATH_RESULT_FLOOD_FALLBACK) {
        entry->out_path_valid = false;
        entry->out_path_len = 0U;
        memset(entry->out_path, 0, sizeof(entry->out_path));
    }
    mark_deferred_persistence_locked(now_ms);
    if (out_entry) {
        *out_entry = *entry;
    }
    if (out_result) {
        *out_result = result;
    }
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

esp_err_t d1l_contact_store_set_flags(const char *fingerprint, bool favorite, bool muted,
                                      d1l_contact_entry_t *out_entry)
{
    if (!fingerprint || fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        esp_err_t ret = d1l_contact_store_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    int existing = find_index_by_fingerprint(fingerprint);
    if (existing < 0) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_NOT_FOUND;
    }

    d1l_contact_entry_t *entry = &s_entries[(size_t)existing];
    if (entry->favorite != favorite || entry->muted != muted) {
        capture_rollback_state();
        const esp_err_t revision_ret = reserve_sequenced_mutation_locked();
        if (revision_ret != ESP_OK) {
            d1l_store_lock_give(&s_store_lock);
            return revision_ret;
        }
        remember_touched_fingerprint_locked(entry->fingerprint);
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        entry->seq = s_next_seq++;
        entry->updated_ms = now_ms;
        entry->favorite = favorite;
        entry->muted = muted;
        esp_err_t ret = persist_store_or_rollback(&s_rollback_scratch);
        if (ret != ESP_OK) {
            d1l_store_lock_give(&s_store_lock);
            return ret;
        }
    }
    if (out_entry) {
        *out_entry = *entry;
    }
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

esp_err_t d1l_contact_store_rename(const char *fingerprint, const char *alias,
                                   d1l_contact_entry_t *out_entry)
{
    if (!fingerprint || fingerprint[0] == '\0' || !alias || alias[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        esp_err_t ret = d1l_contact_store_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    int existing = find_index_by_fingerprint(fingerprint);
    if (existing < 0) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_NOT_FOUND;
    }

    d1l_contact_entry_t *entry = &s_entries[(size_t)existing];
    char sanitized[D1L_CONTACT_ALIAS_LEN] = {0};
    sanitize_ascii(sanitized, sizeof(sanitized), alias);
    if (sanitized[0] == '\0') {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(entry->alias, sanitized, sizeof(entry->alias)) != 0) {
        capture_rollback_state();
        const esp_err_t revision_ret = reserve_sequenced_mutation_locked();
        if (revision_ret != ESP_OK) {
            d1l_store_lock_give(&s_store_lock);
            return revision_ret;
        }
        remember_touched_fingerprint_locked(entry->fingerprint);
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        entry->seq = s_next_seq++;
        entry->updated_ms = now_ms;
        snprintf(entry->alias, sizeof(entry->alias), "%s", sanitized);
        esp_err_t ret = persist_store_or_rollback(&s_rollback_scratch);
        if (ret != ESP_OK) {
            d1l_store_lock_give(&s_store_lock);
            return ret;
        }
    }
    if (out_entry) {
        *out_entry = *entry;
    }
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

esp_err_t d1l_contact_store_delete(const char *fingerprint, d1l_contact_entry_t *out_entry)
{
    if (!fingerprint || fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        esp_err_t ret = d1l_contact_store_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    int existing = find_index_by_fingerprint(fingerprint);
    if (existing < 0) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_NOT_FOUND;
    }

    const size_t index = (size_t)existing;
    d1l_contact_entry_t removed = s_entries[index];
    capture_rollback_state();
    const esp_err_t revision_ret = reserve_persistence_revision_locked(true);
    if (revision_ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        return revision_ret;
    }
    remember_deleted_fingerprint_locked(removed.fingerprint);
    if (index + 1U < s_count) {
        memmove(&s_entries[index], &s_entries[index + 1U],
                (s_count - index - 1U) * sizeof(s_entries[0]));
    }
    s_count--;
    memset(&s_entries[s_count], 0, sizeof(s_entries[s_count]));
    esp_err_t ret = persist_store_or_rollback(&s_rollback_scratch);
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        return ret;
    }
    if (out_entry) {
        *out_entry = removed;
    }
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

uint8_t d1l_contact_store_meshcore_type_id(const char *type)
{
    if (!type) {
        return 0U;
    }
    if (strcmp(type, "chat") == 0) {
        return 1U;
    }
    if (strcmp(type, "repeater") == 0) {
        return 2U;
    }
    if (strcmp(type, "room") == 0) {
        return 3U;
    }
    if (strcmp(type, "sensor") == 0) {
        return 4U;
    }
    return 0U;
}

const char *d1l_contact_store_verification_source_name(uint8_t source)
{
    switch (source) {
    case D1L_CONTACT_VERIFICATION_SIGNED_ADVERT:
        return "signed_advert";
    case D1L_CONTACT_VERIFICATION_URI_IMPORT:
        return "uri_import";
    case D1L_CONTACT_VERIFICATION_MIGRATED_SIGNED_ADVERT:
        return "migrated_signed_advert";
    case D1L_CONTACT_VERIFICATION_NONE:
    default:
        return "none";
    }
}

bool d1l_contact_store_has_export_key(const d1l_contact_entry_t *entry)
{
    if (!entry) {
        return false;
    }
    for (size_t i = 0; i < D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U; ++i) {
        if (!is_hex_char(entry->public_key_hex[i])) {
            return false;
        }
    }
    return entry->public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U] == '\0';
}

bool d1l_contact_store_is_canonical(const d1l_contact_entry_t *entry)
{
    if (!entry ||
        (entry->verification_source !=
             D1L_CONTACT_VERIFICATION_SIGNED_ADVERT &&
         entry->verification_source != D1L_CONTACT_VERIFICATION_URI_IMPORT &&
         entry->verification_source !=
             D1L_CONTACT_VERIFICATION_MIGRATED_SIGNED_ADVERT) ||
        !d1l_contact_store_has_export_key(entry) ||
        !fixed_hex_string_valid(entry->fingerprint,
                                D1L_NODE_FINGERPRINT_LEN - 1U) ||
        !fixed_hex_strings_equal(entry->fingerprint, entry->public_key_hex,
                                 D1L_NODE_FINGERPRINT_LEN - 1U)) {
        return false;
    }
    return d1l_contact_store_meshcore_type_id(entry->type) != 0U;
}

bool d1l_contact_store_can_dm(const d1l_contact_entry_t *entry)
{
    return d1l_contact_store_is_canonical(entry) &&
           strcmp(entry->type, "chat") == 0;
}

bool d1l_contact_store_can_path_probe(const d1l_contact_entry_t *entry)
{
    return d1l_contact_store_is_canonical(entry);
}

bool d1l_contact_store_can_admin(const d1l_contact_entry_t *entry)
{
    return d1l_contact_store_is_canonical(entry) &&
           (strcmp(entry->type, "repeater") == 0 ||
            strcmp(entry->type, "room") == 0);
}

esp_err_t d1l_contact_store_export_uri(const d1l_contact_entry_t *entry, char *dest,
                                       size_t dest_size)
{
    if (!entry || !dest || dest_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    dest[0] = '\0';
    if (!d1l_contact_store_is_canonical(entry)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t type_id = d1l_contact_store_meshcore_type_id(entry->type);
    if (type_id == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    d1l_contact_uri_t contact = {0};
    const char *name = entry->fingerprint;
    size_t name_size = sizeof(entry->fingerprint);
    if (entry->alias[0] != '\0') {
        name = entry->alias;
        name_size = sizeof(entry->alias);
    } else if (entry->heard_name[0] != '\0') {
        name = entry->heard_name;
        name_size = sizeof(entry->heard_name);
    }
    if (!copy_bounded_text(contact.name, sizeof(contact.name), name,
                           name_size)) {
        copy_lower_hex(contact.name, sizeof(contact.name), entry->fingerprint,
                       D1L_NODE_FINGERPRINT_LEN - 1U);
    }
    copy_lower_hex(contact.public_key_hex, sizeof(contact.public_key_hex),
                   entry->public_key_hex,
                   D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U);
    contact.type_id = type_id;
    if (!d1l_contact_uri_format(&contact, dest, dest_size)) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

d1l_contact_store_stats_t d1l_contact_store_stats(void)
{
    d1l_retained_blob_store_backend_state_t backend = {0};
    (void)d1l_retained_blob_store_backend_state(
        D1L_RETAINED_BLOB_STORE_CONTACTS, &backend);
    d1l_store_lock_take(&s_store_lock);
    const bool backend_generation_changed =
        s_loaded && backend.enabled &&
        backend.generation != s_accepted_sd_backend_generation;
    d1l_contact_store_stats_t stats = {
        .next_seq = s_next_seq,
        .total_written = s_total_written,
        .dropped_oldest = s_dropped_oldest,
        .count = s_count,
        .capacity = D1L_CONTACT_STORE_CAPACITY,
        .persistence_revision = __atomic_load_n(
            &s_persistence_revision, __ATOMIC_ACQUIRE),
        .persistence_commit_count = s_persistence_commit_count,
        .persistence_coalesced_count = s_persistence_coalesced_count,
        .persistence_fail_count = s_persistence_fail_count,
        .sd_backend_generation = backend.generation,
        .persistence_last_error = s_persistence_last_error,
        .persistence_dirty =
            s_persistence_dirty ||
            (backend.enabled && s_sd_reconcile_pending) ||
            backend_generation_changed,
        .loaded = s_loaded,
        .sd_primary_required = backend.enabled,
        .sd_primary_reconcile_pending =
            backend.enabled &&
            (s_sd_reconcile_pending || backend_generation_changed),
    };
    d1l_store_lock_give(&s_store_lock);
    return stats;
}

#ifdef D1L_CONTACT_STORE_TEST_HOOKS
esp_err_t d1l_contact_store_test_set_persistence_revision(uint32_t revision)
{
    if (!s_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_store_lock_take(&s_store_lock);
    __atomic_store_n(&s_persistence_revision, revision, __ATOMIC_RELEASE);
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}
#endif

bool d1l_contact_store_find_by_fingerprint(const char *fingerprint, d1l_contact_entry_t *out_entry)
{
    if (!s_loaded && d1l_contact_store_init() != ESP_OK) {
        return false;
    }
    d1l_store_lock_take(&s_store_lock);
    int index = find_index_by_fingerprint(fingerprint);
    if (index < 0) {
        d1l_store_lock_give(&s_store_lock);
        return false;
    }
    if (out_entry) {
        *out_entry = s_entries[index];
    }
    d1l_store_lock_give(&s_store_lock);
    return true;
}

bool d1l_contact_store_find_by_public_key(const char *public_key_hex,
                                          d1l_contact_entry_t *out_entry)
{
    if (!fixed_hex_string_valid(public_key_hex,
                                D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U) ||
        (!s_loaded && d1l_contact_store_init() != ESP_OK)) {
        return false;
    }
    d1l_store_lock_take(&s_store_lock);
    bool ambiguous = false;
    const int index = find_unique_index_by_public_key_hex(
        public_key_hex, &ambiguous);
    if (index < 0 || ambiguous) {
        d1l_store_lock_give(&s_store_lock);
        return false;
    }
    if (out_entry) {
        *out_entry = s_entries[(size_t)index];
    }
    d1l_store_lock_give(&s_store_lock);
    return true;
}

size_t d1l_contact_store_copy_recent(d1l_contact_entry_t *out_entries, size_t max_entries)
{
    if (out_entries == NULL || max_entries == 0) {
        return 0;
    }

    d1l_store_lock_take(&s_store_lock);
    if (s_count == 0) {
        d1l_store_lock_give(&s_store_lock);
        return 0;
    }
    const size_t n = s_count < max_entries ? s_count : max_entries;
    bool used[D1L_CONTACT_STORE_CAPACITY] = {0};
    for (size_t out = 0; out < n; ++out) {
        size_t best = 0;
        bool best_set = false;
        for (size_t i = 0; i < s_count; ++i) {
            if (!used[i] && (!best_set || s_entries[i].seq > s_entries[best].seq)) {
                best = i;
                best_set = true;
            }
        }
        used[best] = true;
        out_entries[out] = s_entries[best];
    }
    d1l_store_lock_give(&s_store_lock);
    return n;
}
