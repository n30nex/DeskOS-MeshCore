#include "node_store.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "nvs.h"

#include "mesh/contact_store.h"
#include "mesh/meshcore_lifetime.h"
#include "mesh/store_lock.h"
#include "storage/retained_blob_store.h"

#define D1L_NODE_STORE_NAMESPACE "d1l_nodes"
#define D1L_NODE_STORE_KEY "heard"
#define D1L_NODE_STORE_EPOCH_KEY "marker_epoch"
#define D1L_NODE_STORE_SD_KEY "nodes_v1"
#define D1L_NODE_STORE_ID D1L_RETAINED_BLOB_STORE_NODES
#define D1L_NODE_STORE_LEGACY_CAPACITY D1L_NODE_NVS_FALLBACK_CAPACITY
#define D1L_NODE_STORE_LEGACY_TYPE_LEN 8U
#define D1L_NODE_STORE_SCHEMA_V1 1U
#define D1L_NODE_STORE_SCHEMA_V2 2U
#define D1L_NODE_STORE_SCHEMA_V3 3U
#define D1L_NODE_STORE_SCHEMA_V4 4U
#define D1L_NODE_STORE_SD_SCHEMA 5U

typedef struct {
    uint32_t seq;
    uint32_t first_heard_ms;
    uint32_t last_heard_ms;
    uint32_t advert_timestamp;
    uint32_t heard_count;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char name[D1L_HEARD_NODE_NAME_LEN];
    char type[D1L_NODE_STORE_LEGACY_TYPE_LEN];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
} d1l_node_entry_v1_t;

/* Schema v2/v3 entries predate signed-advert locations. Keep this layout exact. */
typedef struct {
    uint32_t seq;
    uint32_t first_heard_ms;
    uint32_t last_heard_ms;
    uint32_t advert_timestamp;
    uint32_t heard_count;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    char name[D1L_HEARD_NODE_NAME_LEN];
    char type[D1L_NODE_STORE_LEGACY_TYPE_LEN];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
} d1l_node_entry_v3_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_node_entry_v1_t entries[D1L_NODE_STORE_LEGACY_CAPACITY];
} d1l_node_store_blob_v1_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_node_entry_v3_t entries[D1L_NODE_STORE_LEGACY_CAPACITY];
} d1l_node_store_blob_v2_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_node_entry_v3_t entries[D1L_NODE_STORE_LEGACY_CAPACITY];
} d1l_node_store_blob_v3_t;

_Static_assert(sizeof(d1l_node_entry_v1_t) == 84U,
               "node schema v1 layout changed");
_Static_assert(sizeof(d1l_node_entry_v3_t) == 148U,
               "node schema v2/v3 layout changed");
_Static_assert(offsetof(d1l_node_entry_v1_t, rssi_dbm) == 72U,
               "node schema v1 type offset changed");
_Static_assert(offsetof(d1l_node_entry_v3_t, rssi_dbm) == 136U,
               "node schema v2/v3 type offset changed");

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_node_entry_t entries[D1L_NODE_NVS_FALLBACK_CAPACITY];
} d1l_node_store_blob_v4_t;

typedef struct {
    uint32_t schema;
    uint32_t epoch;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_node_entry_t entries[D1L_NODE_SD_HISTORY_CAPACITY];
} d1l_node_store_sd_blob_t;

static d1l_node_entry_t s_entries[D1L_NODE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
/* Monotonic uptime is meaningful only within one boot.  Retain the historical
 * entry timestamps for display/audit, but derive live reachability solely from
 * this boot-local observation set. */
static uint32_t s_live_last_heard_ms[D1L_NODE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static bool s_live_heard_valid[D1L_NODE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static size_t s_count;
static uint32_t s_next_seq = 1;
static uint32_t s_total_written;
static uint32_t s_dropped_oldest;
static uint32_t s_marker_generation = 1U;
static uint32_t s_epoch = 1U;
static bool s_loaded;
static d1l_node_store_blob_v4_t s_legacy_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_node_store_sd_blob_t s_sd_blob_scratch EXT_RAM_BSS_ATTR;
static d1l_node_store_sd_blob_t s_persist_snapshot EXT_RAM_BSS_ATTR;
static d1l_node_view_t s_query_scratch[D1L_NODE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static bool s_persistence_dirty;
static bool s_sd_reconcile_pending;
static bool s_dirty_timing_started;
static bool s_persistence_immediate_due;
static bool s_retry_pending;
static bool s_legacy_cleanup_pending;
static uint32_t s_dirty_since_ms;
static uint32_t s_last_persist_attempt_ms;
static uint32_t s_last_sd_backend_generation;
static uint32_t s_persistence_commit_count;
static uint32_t s_persistence_coalesced_count;
static uint32_t s_persistence_fail_count;
static esp_err_t s_sd_primary_last_error;
static uint64_t s_revision;
static d1l_store_lock_t s_store_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_persist_io_lock = D1L_STORE_LOCK_INITIALIZER;

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

static char lower_hex_char(char value)
{
    if (value >= 'A' && value <= 'F') {
        return (char)(value + ('a' - 'A'));
    }
    return value;
}

static bool public_keys_equal(const char *left, const char *right)
{
    if (!left || !right) {
        return false;
    }
    for (size_t i = 0U; i < D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U; ++i) {
        if (left[i] == '\0' || right[i] == '\0' ||
            lower_hex_char(left[i]) != lower_hex_char(right[i])) {
            return false;
        }
    }
    return left[D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U] == '\0' &&
           right[D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U] == '\0';
}

static void migrate_legacy_advert_type(
    char dest[D1L_NODE_TYPE_LEN],
    const char legacy_type[D1L_NODE_STORE_LEGACY_TYPE_LEN])
{
    char legacy[D1L_NODE_STORE_LEGACY_TYPE_LEN + 1U] = {0};
    memcpy(legacy, legacy_type, D1L_NODE_STORE_LEGACY_TYPE_LEN);
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

static const char *type_name(char type_code)
{
    switch (type_code) {
    case 'C':
        return "chat";
    case 'P':
        return "repeater";
    case 'R':
        return "room";
    case 'S':
        return "sensor";
    default:
        return "node";
    }
}

static void clear_ram(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_live_last_heard_ms, 0, sizeof(s_live_last_heard_ms));
    memset(s_live_heard_valid, 0, sizeof(s_live_heard_valid));
    s_count = 0;
    s_next_seq = 1;
    s_total_written = 0;
    s_dropped_oldest = 0;
}

static void bump_marker_generation(void)
{
    s_marker_generation++;
    if (s_marker_generation == 0U) {
        s_marker_generation = 1U;
    }
}

static bool location_in_bounds(int32_t lat_e6, int32_t lon_e6)
{
    return lat_e6 >= -90000000 && lat_e6 <= 90000000 &&
           lon_e6 >= -180000000 && lon_e6 <= 180000000;
}

static bool marker_material_changed(const d1l_node_entry_t *before,
                                    const d1l_node_entry_t *after)
{
    const bool before_valid = before && before->location_valid;
    const bool after_valid = after && after->location_valid;
    if (before_valid != after_valid) {
        return true;
    }
    if (!before_valid) {
        return false;
    }
    return before->lat_e6 != after->lat_e6 || before->lon_e6 != after->lon_e6 ||
           strncmp(before->fingerprint, after->fingerprint, D1L_NODE_FINGERPRINT_LEN) != 0 ||
           strncmp(before->name, after->name, D1L_HEARD_NODE_NAME_LEN) != 0 ||
           strncmp(before->type, after->type, D1L_NODE_TYPE_LEN) != 0;
}

static uint32_t monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void reset_persistence_state(uint32_t backend_generation)
{
    s_persistence_dirty = false;
    s_sd_reconcile_pending = false;
    s_dirty_timing_started = false;
    s_persistence_immediate_due = false;
    s_retry_pending = false;
    s_legacy_cleanup_pending = false;
    s_dirty_since_ms = 0U;
    s_last_persist_attempt_ms = 0U;
    s_last_sd_backend_generation = backend_generation;
    s_persistence_commit_count = 0U;
    s_persistence_coalesced_count = 0U;
    s_persistence_fail_count = 0U;
    s_sd_primary_last_error = ESP_OK;
    s_revision = 1U;
}

static void note_persistence_dirty_locked(bool immediate, uint32_t now_ms)
{
    if (!s_dirty_timing_started) {
        s_dirty_timing_started = true;
        s_dirty_since_ms = now_ms;
    }
    s_persistence_dirty = true;
    s_persistence_immediate_due = s_persistence_immediate_due || immediate;
}

static void fill_sd_blob(d1l_node_store_sd_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->schema = D1L_NODE_STORE_SD_SCHEMA;
    blob->epoch = s_epoch;
    blob->next_seq = s_next_seq;
    blob->total_written = s_total_written;
    blob->dropped_oldest = s_dropped_oldest;
    blob->count = (uint32_t)s_count;
    if (s_count > 0U) {
        memcpy(blob->entries, s_entries,
               s_count * sizeof(blob->entries[0]));
    }
}

static esp_err_t load_clear_epoch(uint32_t *out_epoch)
{
    if (!out_epoch) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_epoch = 1U;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_NODE_STORE_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t epoch = 0U;
    ret = nvs_get_u32(handle, D1L_NODE_STORE_EPOCH_KEY, &epoch);
    nvs_close(handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (epoch == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_epoch = epoch;
    return ESP_OK;
}

static esp_err_t store_clear_epoch(uint32_t epoch)
{
    if (epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_NODE_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u32(handle, D1L_NODE_STORE_EPOCH_KEY, epoch);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t erase_legacy_node_blob(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_NODE_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_erase_key(handle, D1L_NODE_STORE_KEY);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static bool sd_blob_is_valid(const d1l_node_store_sd_blob_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_NODE_STORE_SD_SCHEMA &&
           blob->epoch > 0U &&
           blob->count <= D1L_NODE_SD_HISTORY_CAPACITY &&
           blob->next_seq > 0U;
}

static bool blob_v4_is_valid(const d1l_node_store_blob_v4_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_NODE_STORE_SCHEMA_V4 &&
           blob->count <= D1L_NODE_NVS_FALLBACK_CAPACITY &&
           blob->next_seq > 0;
}

static bool blob_v1_is_valid(const d1l_node_store_blob_v1_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_NODE_STORE_SCHEMA_V1 &&
           blob->count <= D1L_NODE_STORE_LEGACY_CAPACITY &&
           blob->next_seq > 0;
}

static bool blob_v2_is_valid(const d1l_node_store_blob_v2_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_NODE_STORE_SCHEMA_V2 &&
           blob->count <= D1L_NODE_STORE_LEGACY_CAPACITY &&
           blob->next_seq > 0;
}

static bool blob_v3_is_valid(const d1l_node_store_blob_v3_t *blob, size_t len)
{
    return blob && len == sizeof(*blob) &&
           blob->schema == D1L_NODE_STORE_SCHEMA_V3 &&
           blob->count <= D1L_NODE_STORE_LEGACY_CAPACITY &&
           blob->next_seq > 0;
}

static void migrate_v1_blob(const d1l_node_store_blob_v1_t *old_blob)
{
    clear_ram();
    s_count = old_blob->count;
    s_next_seq = old_blob->next_seq;
    s_total_written = old_blob->total_written;
    s_dropped_oldest = old_blob->dropped_oldest;
    for (size_t i = 0; i < s_count; ++i) {
        s_entries[i].seq = old_blob->entries[i].seq;
        s_entries[i].first_heard_ms = old_blob->entries[i].first_heard_ms;
        s_entries[i].last_heard_ms = old_blob->entries[i].last_heard_ms;
        s_entries[i].advert_timestamp = old_blob->entries[i].advert_timestamp;
        s_entries[i].heard_count = old_blob->entries[i].heard_count;
        memcpy(s_entries[i].fingerprint, old_blob->entries[i].fingerprint,
               sizeof(s_entries[i].fingerprint));
        memcpy(s_entries[i].name, old_blob->entries[i].name, sizeof(s_entries[i].name));
        migrate_legacy_advert_type(s_entries[i].type, old_blob->entries[i].type);
        s_entries[i].rssi_dbm = old_blob->entries[i].rssi_dbm;
        s_entries[i].snr_tenths = old_blob->entries[i].snr_tenths;
        s_entries[i].path_hash_bytes = old_blob->entries[i].path_hash_bytes;
        s_entries[i].path_hops = old_blob->entries[i].path_hops;
    }
}

static void migrate_v3_entries(const d1l_node_entry_v3_t *old_entries, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        s_entries[i].seq = old_entries[i].seq;
        s_entries[i].first_heard_ms = old_entries[i].first_heard_ms;
        s_entries[i].last_heard_ms = old_entries[i].last_heard_ms;
        s_entries[i].advert_timestamp = old_entries[i].advert_timestamp;
        s_entries[i].heard_count = old_entries[i].heard_count;
        memcpy(s_entries[i].fingerprint, old_entries[i].fingerprint,
               sizeof(s_entries[i].fingerprint));
        memcpy(s_entries[i].public_key_hex, old_entries[i].public_key_hex,
               sizeof(s_entries[i].public_key_hex));
        memcpy(s_entries[i].name, old_entries[i].name, sizeof(s_entries[i].name));
        migrate_legacy_advert_type(s_entries[i].type, old_entries[i].type);
        s_entries[i].rssi_dbm = old_entries[i].rssi_dbm;
        s_entries[i].snr_tenths = old_entries[i].snr_tenths;
        s_entries[i].path_hash_bytes = old_entries[i].path_hash_bytes;
        s_entries[i].path_hops = old_entries[i].path_hops;
    }
}

static void migrate_v2_blob(const d1l_node_store_blob_v2_t *old_blob)
{
    clear_ram();
    s_count = old_blob->count;
    s_next_seq = old_blob->next_seq;
    s_total_written = old_blob->total_written;
    s_dropped_oldest = old_blob->dropped_oldest;
    migrate_v3_entries(old_blob->entries, s_count);
}

static void migrate_v3_blob(const d1l_node_store_blob_v3_t *old_blob)
{
    clear_ram();
    s_count = old_blob->count;
    s_next_seq = old_blob->next_seq;
    s_total_written = old_blob->total_written;
    s_dropped_oldest = old_blob->dropped_oldest;
    migrate_v3_entries(old_blob->entries, s_count);
}

static int find_by_fingerprint(const char *fingerprint)
{
    if (!fingerprint || fingerprint[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < s_count; ++i) {
        if (strncmp(s_entries[i].fingerprint, fingerprint, sizeof(s_entries[i].fingerprint)) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool oldest_unlocated_index(size_t *out_index)
{
    if (!out_index) {
        return false;
    }
    bool found = false;
    size_t oldest = 0U;
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[i].location_valid) {
            continue;
        }
        if (!found || s_entries[i].seq < s_entries[oldest].seq) {
            oldest = i;
            found = true;
        }
    }
    if (found) {
        *out_index = oldest;
    }
    return found;
}

static bool bounded_text_terminated(const char *text, size_t capacity)
{
    return text && capacity > 0U && memchr(text, '\0', capacity) != NULL;
}

static bool persisted_entry_is_valid(const d1l_node_entry_t *entry)
{
    return entry &&
           bounded_text_terminated(entry->fingerprint,
                                   sizeof(entry->fingerprint)) &&
           bounded_text_terminated(entry->public_key_hex,
                                   sizeof(entry->public_key_hex)) &&
           bounded_text_terminated(entry->name, sizeof(entry->name)) &&
           bounded_text_terminated(entry->type, sizeof(entry->type)) &&
           entry->fingerprint[0] != '\0' &&
           (!entry->location_valid ||
            location_in_bounds(entry->lat_e6, entry->lon_e6));
}

static bool least_recent_unlocated_advert_index(size_t *out_index)
{
    if (!out_index) {
        return false;
    }
    bool found = false;
    size_t oldest = 0U;
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[i].location_valid) {
            continue;
        }
        if (!found ||
            s_entries[i].advert_timestamp <
                s_entries[oldest].advert_timestamp ||
            (s_entries[i].advert_timestamp ==
                 s_entries[oldest].advert_timestamp &&
             s_entries[i].seq < s_entries[oldest].seq)) {
            oldest = i;
            found = true;
        }
    }
    if (found) {
        *out_index = oldest;
    }
    return found;
}

static bool merge_persisted_entry_locked(const d1l_node_entry_t *incoming,
                                         bool *out_marker_changed)
{
    if (!persisted_entry_is_valid(incoming)) {
        return false;
    }
    if (out_marker_changed) {
        *out_marker_changed = false;
    }

    int existing = find_by_fingerprint(incoming->fingerprint);
    if (existing < 0) {
        size_t index = 0U;
        if (s_count < D1L_NODE_STORE_CAPACITY) {
            index = s_count++;
        } else {
            /*
             * A signed advert with coordinates remains a Map marker until the
             * user clears the node store. Capacity pressure may replace only
             * an entry that has never supplied a valid location.
             */
            if (!least_recent_unlocated_advert_index(&index)) {
                return false;
            }
            if (incoming->advert_timestamp <
                    s_entries[index].advert_timestamp ||
                (incoming->advert_timestamp ==
                     s_entries[index].advert_timestamp &&
                 incoming->seq <= s_entries[index].seq)) {
                return false;
            }
            s_dropped_oldest++;
        }
        const d1l_node_entry_t before = s_entries[index];
        s_entries[index] = *incoming;
        s_live_last_heard_ms[index] = 0U;
        s_live_heard_valid[index] = false;
        if (out_marker_changed) {
            *out_marker_changed = marker_material_changed(&before,
                                                          &s_entries[index]);
        }
        return true;
    }

    const size_t index = (size_t)existing;
    d1l_node_entry_t *current = &s_entries[index];
    const d1l_node_entry_t before = *current;
    bool changed = false;
    if (incoming->advert_timestamp > current->advert_timestamp) {
        const bool preserve_location =
            current->location_valid &&
            (!incoming->location_valid ||
             current->location_advert_timestamp >
                 incoming->location_advert_timestamp);
        const int32_t lat_e6 = current->lat_e6;
        const int32_t lon_e6 = current->lon_e6;
        const uint32_t location_timestamp =
            current->location_advert_timestamp;
        const uint32_t location_seq = current->location_seq;
        *current = *incoming;
        if (preserve_location) {
            current->location_valid = true;
            current->lat_e6 = lat_e6;
            current->lon_e6 = lon_e6;
            current->location_advert_timestamp = location_timestamp;
            current->location_seq = location_seq;
        }
        changed = true;
    } else if (incoming->advert_timestamp == current->advert_timestamp) {
        if (incoming->location_valid &&
            (!current->location_valid ||
             incoming->location_advert_timestamp >
                 current->location_advert_timestamp)) {
            current->location_valid = true;
            current->lat_e6 = incoming->lat_e6;
            current->lon_e6 = incoming->lon_e6;
            current->location_advert_timestamp =
                incoming->location_advert_timestamp;
            current->location_seq = incoming->location_seq;
            changed = true;
        }
        if (current->public_key_hex[0] == '\0' &&
            incoming->public_key_hex[0] != '\0') {
            memcpy(current->public_key_hex, incoming->public_key_hex,
                   sizeof(current->public_key_hex));
            changed = true;
        }
        if (current->name[0] == '\0' && incoming->name[0] != '\0') {
            memcpy(current->name, incoming->name, sizeof(current->name));
            changed = true;
        }
    }
    if (changed && out_marker_changed) {
        *out_marker_changed = marker_material_changed(&before, current);
    }
    return changed;
}

static bool merge_sd_blob_locked(const d1l_node_store_sd_blob_t *blob)
{
    if (!blob || blob->epoch != s_epoch) {
        return false;
    }
    bool changed = false;
    bool markers_changed = false;
    for (size_t i = 0U; i < blob->count; ++i) {
        bool marker_changed = false;
        if (merge_persisted_entry_locked(&blob->entries[i],
                                         &marker_changed)) {
            changed = true;
            markers_changed = markers_changed || marker_changed;
        }
    }
    if (blob->next_seq > s_next_seq) {
        s_next_seq = blob->next_seq;
    }
    if (blob->total_written > s_total_written) {
        s_total_written = blob->total_written;
    }
    if (blob->dropped_oldest > s_dropped_oldest) {
        s_dropped_oldest = blob->dropped_oldest;
    }
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[i].seq >= s_next_seq && s_entries[i].seq < UINT32_MAX) {
            s_next_seq = s_entries[i].seq + 1U;
        }
    }
    if (s_next_seq == 0U) {
        s_next_seq = 1U;
    }
    if (markers_changed) {
        bump_marker_generation();
    }
    return changed;
}

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static int ascii_casecmp(const char *a, const char *b)
{
    while ((a && *a) || (b && *b)) {
        const char ca = ascii_lower(a && *a ? *a++ : '\0');
        const char cb = ascii_lower(b && *b ? *b++ : '\0');
        if (ca != cb) {
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
        }
    }
    return 0;
}

static bool contains_casefold(const char *haystack, const char *needle)
{
    if (!needle || needle[0] == '\0') {
        return true;
    }
    if (!haystack) {
        return false;
    }
    for (size_t i = 0; haystack[i] != '\0'; ++i) {
        size_t h = i;
        size_t n = 0;
        while (haystack[h] != '\0' && needle[n] != '\0' &&
               ascii_lower(haystack[h]) == ascii_lower(needle[n])) {
            h++;
            n++;
        }
        if (needle[n] == '\0') {
            return true;
        }
    }
    return false;
}

static bool node_has_key(const d1l_node_entry_t *node, const d1l_contact_entry_t *contact)
{
    return (node && node->public_key_hex[0] != '\0') ||
           (contact && contact->public_key_hex[0] != '\0');
}

static const char *node_role_name(const d1l_node_entry_t *node)
{
    if (!node) {
        return "unknown";
    }
    if (strcmp(node->type, "room") == 0) {
        return "room";
    }
    if (strcmp(node->type, "sensor") == 0) {
        return "sensor";
    }
    if (strcmp(node->type, "repeater") == 0) {
        return "repeater";
    }
    if (strcmp(node->type, "chat") == 0) {
        return "companion";
    }
    return "unknown";
}

static uint8_t node_role_order(const char *role)
{
    if (!role) {
        return 5U;
    }
    if (strcmp(role, "companion") == 0) {
        return 0U;
    }
    if (strcmp(role, "repeater") == 0) {
        return 1U;
    }
    if (strcmp(role, "room") == 0) {
        return 2U;
    }
    if (strcmp(role, "sensor") == 0) {
        return 3U;
    }
    return 4U;
}

static void build_node_view(size_t index, const d1l_node_entry_t *node,
                            d1l_node_view_t *view, uint32_t now_ms)
{
    if (index >= s_count || !node || !view) {
        return;
    }
    memset(view, 0, sizeof(*view));
    view->node = *node;

    d1l_contact_entry_t contact = {0};
    const bool has_contact =
        d1l_contact_store_find_by_fingerprint(node->fingerprint, &contact);
    view->favorite = has_contact && contact.favorite;
    view->muted = has_contact && contact.muted;
    view->keyed = node_has_key(node, has_contact ? &contact : NULL);
    view->reachable = s_live_heard_valid[index] &&
        d1l_meshcore_lifetime_age_current_u32(
            s_live_last_heard_ms[index], now_ms,
            D1L_MESHCORE_CONTACT_REACHABLE_MAX_AGE_MS);
    sanitize_ascii(view->role, sizeof(view->role), node_role_name(node));
    if (has_contact && contact.alias[0] != '\0') {
        sanitize_ascii(view->display_name, sizeof(view->display_name), contact.alias);
    } else if (node->name[0] != '\0') {
        sanitize_ascii(view->display_name, sizeof(view->display_name), node->name);
    } else {
        sanitize_ascii(view->display_name, sizeof(view->display_name), node->fingerprint);
    }
}

static bool node_view_matches_filter(const d1l_node_view_t *view, d1l_node_filter_t filter)
{
    if (!view) {
        return false;
    }
    switch (filter) {
    case D1L_NODE_FILTER_ALL:
        return true;
    case D1L_NODE_FILTER_COMPANION:
        return strcmp(view->role, "companion") == 0;
    case D1L_NODE_FILTER_REPEATER:
        return strcmp(view->role, "repeater") == 0;
    case D1L_NODE_FILTER_ROOM:
        return strcmp(view->role, "room") == 0;
    case D1L_NODE_FILTER_SENSOR:
        return strcmp(view->role, "sensor") == 0;
    case D1L_NODE_FILTER_FAVORITE:
        return view->favorite;
    default:
        return true;
    }
}

static bool node_view_matches_query(const d1l_node_view_t *view, const d1l_node_query_t *query)
{
    if (!view) {
        return false;
    }
    if (query) {
        if (!node_view_matches_filter(view, query->filter)) {
            return false;
        }
        if (query->keyed_only && !view->keyed) {
            return false;
        }
        if (query->reachable_only && !view->reachable) {
            return false;
        }
        if (query->text && query->text[0] != '\0' &&
            !contains_casefold(view->display_name, query->text) &&
            !contains_casefold(view->role, query->text) &&
            !contains_casefold(view->node.fingerprint, query->text) &&
            !contains_casefold(view->node.type, query->text) &&
            !contains_casefold(view->node.public_key_hex, query->text)) {
            return false;
        }
    }
    return true;
}

static bool node_view_better(const d1l_node_view_t *candidate, const d1l_node_view_t *best,
                             d1l_node_sort_t sort)
{
    if (!best) {
        return true;
    }
    switch (sort) {
    case D1L_NODE_SORT_SIGNAL:
        if (candidate->node.rssi_dbm != best->node.rssi_dbm) {
            return candidate->node.rssi_dbm > best->node.rssi_dbm;
        }
        if (candidate->node.snr_tenths != best->node.snr_tenths) {
            return candidate->node.snr_tenths > best->node.snr_tenths;
        }
        break;
    case D1L_NODE_SORT_NAME: {
        int cmp = ascii_casecmp(candidate->display_name, best->display_name);
        if (cmp != 0) {
            return cmp < 0;
        }
        break;
    }
    case D1L_NODE_SORT_ROLE: {
        uint8_t candidate_role = node_role_order(candidate->role);
        uint8_t best_role = node_role_order(best->role);
        if (candidate_role != best_role) {
            return candidate_role < best_role;
        }
        int cmp = ascii_casecmp(candidate->display_name, best->display_name);
        if (cmp != 0) {
            return cmp < 0;
        }
        break;
    }
    case D1L_NODE_SORT_FAVORITE:
        if (candidate->favorite != best->favorite) {
            return candidate->favorite;
        }
        break;
    case D1L_NODE_SORT_LAST_HEARD:
    default:
        break;
    }
    if (candidate->node.last_heard_ms != best->node.last_heard_ms) {
        return candidate->node.last_heard_ms > best->node.last_heard_ms;
    }
    return candidate->node.seq > best->node.seq;
}

esp_err_t d1l_node_store_init(void)
{
    d1l_store_lock_take(&s_persist_io_lock);

    d1l_retained_blob_store_backend_state_t backend = {0};
    if (!d1l_retained_blob_store_backend_state(D1L_NODE_STORE_ID,
                                               &backend)) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t epoch = 1U;
    esp_err_t ret = load_clear_epoch(&epoch);
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    s_marker_generation = 1U;
    clear_ram();
    s_epoch = epoch;
    reset_persistence_state(backend.generation);
    s_loaded = false;
    d1l_store_lock_give(&s_store_lock);

    bool legacy_found = false;
    nvs_handle_t handle;
    ret = nvs_open(D1L_NODE_STORE_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_OK) {
        size_t len = sizeof(s_legacy_blob_scratch);
        ret = nvs_get_blob(handle, D1L_NODE_STORE_KEY,
                           &s_legacy_blob_scratch, &len);
        nvs_close(handle);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        } else if (ret == ESP_OK) {
            d1l_store_lock_take(&s_store_lock);
            if (blob_v4_is_valid(&s_legacy_blob_scratch, len)) {
                memcpy(s_entries, s_legacy_blob_scratch.entries,
                       s_legacy_blob_scratch.count * sizeof(s_entries[0]));
                s_count = s_legacy_blob_scratch.count;
                s_next_seq = s_legacy_blob_scratch.next_seq;
                s_total_written = s_legacy_blob_scratch.total_written;
                s_dropped_oldest = s_legacy_blob_scratch.dropped_oldest;
                legacy_found = true;
            } else if (blob_v3_is_valid(
                           (const d1l_node_store_blob_v3_t *)
                               &s_legacy_blob_scratch, len)) {
                migrate_v3_blob((const d1l_node_store_blob_v3_t *)
                                    &s_legacy_blob_scratch);
                legacy_found = true;
            } else if (blob_v2_is_valid(
                           (const d1l_node_store_blob_v2_t *)
                               &s_legacy_blob_scratch, len)) {
                migrate_v2_blob((const d1l_node_store_blob_v2_t *)
                                    &s_legacy_blob_scratch);
                legacy_found = true;
            } else if (blob_v1_is_valid(
                           (const d1l_node_store_blob_v1_t *)
                               &s_legacy_blob_scratch, len)) {
                migrate_v1_blob((const d1l_node_store_blob_v1_t *)
                                    &s_legacy_blob_scratch);
                legacy_found = true;
            } else {
                ret = ESP_ERR_INVALID_STATE;
            }
            d1l_store_lock_give(&s_store_lock);
        }
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        d1l_store_lock_take(&s_store_lock);
        s_persistence_fail_count++;
        s_sd_primary_last_error = ret;
        s_loaded = true;
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }

    bool sd_valid = false;
    if (backend.enabled) {
        size_t sd_len = sizeof(s_sd_blob_scratch);
        const esp_err_t sd_ret = d1l_retained_blob_store_read_sd_primary(
            D1L_NODE_STORE_ID, D1L_NODE_STORE_SD_KEY,
            &s_sd_blob_scratch, &sd_len);
        sd_valid = sd_ret == ESP_OK &&
            sd_blob_is_valid(&s_sd_blob_scratch, sd_len);
        d1l_store_lock_take(&s_store_lock);
        if (sd_valid && s_sd_blob_scratch.epoch == s_epoch) {
            (void)merge_sd_blob_locked(&s_sd_blob_scratch);
            s_sd_reconcile_pending = false;
            s_sd_primary_last_error = ESP_OK;
        } else if (sd_ret == ESP_ERR_NOT_FOUND ||
                   (sd_valid && s_sd_blob_scratch.epoch != s_epoch)) {
            s_sd_reconcile_pending = false;
            if (s_count > 0U || s_epoch > 1U) {
                note_persistence_dirty_locked(true, monotonic_ms());
            }
        } else {
            s_sd_reconcile_pending = true;
            s_persistence_fail_count++;
            s_sd_primary_last_error =
                sd_ret == ESP_OK ? ESP_ERR_INVALID_STATE : sd_ret;
        }
        d1l_store_lock_give(&s_store_lock);
    } else {
        d1l_store_lock_take(&s_store_lock);
        s_sd_reconcile_pending = true;
        if (legacy_found) {
            note_persistence_dirty_locked(false, monotonic_ms());
        }
        d1l_store_lock_give(&s_store_lock);
    }

    d1l_store_lock_take(&s_store_lock);
    s_legacy_cleanup_pending = legacy_found;
    if (legacy_found) {
        note_persistence_dirty_locked(true, monotonic_ms());
    }
    s_loaded = true;
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    return ESP_OK;
}

esp_err_t d1l_node_store_clear(void)
{
    if (!s_loaded) {
        const esp_err_t init_ret = d1l_node_store_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }
    d1l_store_lock_take(&s_persist_io_lock);
    d1l_retained_blob_store_backend_state_t backend = {0};
    if (!d1l_retained_blob_store_backend_state(D1L_NODE_STORE_ID,
                                               &backend)) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    d1l_store_lock_take(&s_store_lock);
    uint32_t clear_epoch = s_epoch + 1U;
    if (clear_epoch == 0U) {
        clear_epoch = 1U;
    }
    d1l_store_lock_give(&s_store_lock);
    esp_err_t ret = store_clear_epoch(clear_epoch);
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    bool had_markers = false;
    for (size_t i = 0; i < s_count; ++i) {
        had_markers = had_markers || s_entries[i].location_valid;
    }
    clear_ram();
    s_epoch = clear_epoch;
    s_revision++;
    s_sd_reconcile_pending = !backend.enabled;
    note_persistence_dirty_locked(true, monotonic_ms());
    if (had_markers) {
        bump_marker_generation();
    }
    s_loaded = true;
    fill_sd_blob(&s_persist_snapshot);
    d1l_store_lock_give(&s_store_lock);

    esp_err_t sd_ret = ESP_OK;
    if (backend.enabled) {
        sd_ret = d1l_retained_blob_store_write_sd_primary_guarded(
            D1L_NODE_STORE_ID, D1L_NODE_STORE_SD_KEY,
            &s_persist_snapshot, sizeof(s_persist_snapshot),
            backend.generation);
    }
    const esp_err_t legacy_ret = erase_legacy_node_blob();

    d1l_store_lock_take(&s_store_lock);
    s_legacy_cleanup_pending = legacy_ret != ESP_OK;
    if (backend.enabled && sd_ret == ESP_OK) {
        s_persistence_dirty = false;
        s_sd_reconcile_pending = false;
        s_dirty_timing_started = false;
        s_persistence_immediate_due = false;
        s_retry_pending = false;
        s_persistence_commit_count++;
        s_sd_primary_last_error = ESP_OK;
    } else if (backend.enabled) {
        s_persistence_fail_count++;
        s_sd_primary_last_error = sd_ret;
        s_retry_pending = true;
    }
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    /* The committed epoch makes the clear durable even when SD is absent or
     * temporarily unavailable. The worker replaces that card's old snapshot
     * only after the same epoch is observed again. */
    return legacy_ret;
}

static bool node_sd_backend_generation_matches(uint32_t expected_generation)
{
    d1l_retained_blob_store_backend_state_t backend = {0};
    return d1l_retained_blob_store_backend_state(D1L_NODE_STORE_ID,
                                                 &backend) &&
           backend.enabled && backend.generation == expected_generation;
}

static esp_err_t note_sd_failure(esp_err_t failure)
{
    d1l_store_lock_take(&s_store_lock);
    s_persistence_fail_count++;
    s_sd_primary_last_error = failure;
    s_retry_pending = true;
    s_sd_reconcile_pending = true;
    d1l_store_lock_give(&s_store_lock);
    return failure;
}

static esp_err_t reconcile_sd_primary(uint32_t expected_generation)
{
    size_t len = sizeof(s_sd_blob_scratch);
    esp_err_t ret = d1l_retained_blob_store_read_sd_primary(
        D1L_NODE_STORE_ID, D1L_NODE_STORE_SD_KEY,
        &s_sd_blob_scratch, &len);
    if (ret == ESP_ERR_NOT_FINISHED) {
        return ret;
    }
    if (ret == ESP_ERR_NOT_FOUND) {
        d1l_store_lock_take(&s_store_lock);
        s_sd_reconcile_pending = false;
        s_sd_primary_last_error = ESP_OK;
        if (s_count > 0U || s_epoch > 1U) {
            note_persistence_dirty_locked(true, monotonic_ms());
        }
        d1l_store_lock_give(&s_store_lock);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return note_sd_failure(ret);
    }
    if (!sd_blob_is_valid(&s_sd_blob_scratch, len)) {
        return note_sd_failure(ESP_ERR_INVALID_STATE);
    }
    if (!node_sd_backend_generation_matches(expected_generation)) {
        return note_sd_failure(ESP_ERR_INVALID_STATE);
    }

    d1l_store_lock_take(&s_store_lock);
    if (s_sd_blob_scratch.epoch != s_epoch) {
        /* The tiny onboard epoch is the authority for an explicit clear.
         * Never merge a card from an older or unexplained newer epoch. */
        s_sd_reconcile_pending = false;
        s_sd_primary_last_error = ESP_OK;
        note_persistence_dirty_locked(true, monotonic_ms());
        d1l_store_lock_give(&s_store_lock);
        return ESP_OK;
    }
    if (merge_sd_blob_locked(&s_sd_blob_scratch)) {
        s_revision++;
    }
    s_sd_reconcile_pending = false;
    s_sd_primary_last_error = ESP_OK;
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

static esp_err_t persist_node_snapshot(bool force)
{
    d1l_store_lock_take(&s_persist_io_lock);
    d1l_retained_blob_store_backend_state_t backend = {0};
    if (!d1l_retained_blob_store_backend_state(D1L_NODE_STORE_ID,
                                               &backend)) {
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t now_ms = monotonic_ms();

    d1l_store_lock_take(&s_store_lock);
    if (!s_loaded) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (backend.generation != s_last_sd_backend_generation) {
        s_last_sd_backend_generation = backend.generation;
        s_sd_reconcile_pending = true;
        s_persistence_immediate_due = true;
    }
    if (!backend.enabled) {
        s_sd_reconcile_pending = true;
        if (s_persistence_dirty || s_legacy_cleanup_pending) {
            s_persistence_coalesced_count++;
        }
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }

    const bool pending = s_persistence_dirty ||
        s_sd_reconcile_pending || s_legacy_cleanup_pending;
    if (!pending) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }
    const uint32_t elapsed_since_attempt =
        now_ms - s_last_persist_attempt_ms;
    const bool retry_ready = !s_retry_pending ||
        elapsed_since_attempt >= D1L_NODE_STORE_PERSIST_MIN_INTERVAL_MS;
    const bool min_due = s_dirty_timing_started &&
        now_ms - s_dirty_since_ms >=
            D1L_NODE_STORE_PERSIST_MIN_INTERVAL_MS;
    const bool max_due = s_dirty_timing_started &&
        now_ms - s_dirty_since_ms >=
            D1L_NODE_STORE_PERSIST_MAX_INTERVAL_MS;
    if (!force && !s_persistence_immediate_due && !min_due && !max_due) {
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return ESP_OK;
    }
    if (!retry_ready) {
        const esp_err_t retry_error = s_sd_primary_last_error == ESP_OK ?
            ESP_ERR_INVALID_STATE : s_sd_primary_last_error;
        d1l_store_lock_give(&s_store_lock);
        d1l_store_lock_give(&s_persist_io_lock);
        return force ? retry_error : ESP_OK;
    }
    const bool reconcile = s_sd_reconcile_pending;
    s_last_persist_attempt_ms = now_ms;
    s_persistence_immediate_due = false;
    d1l_store_lock_give(&s_store_lock);

    if (reconcile) {
        const esp_err_t reconcile_ret =
            reconcile_sd_primary(backend.generation);
        if (reconcile_ret != ESP_OK) {
            d1l_store_lock_give(&s_persist_io_lock);
            return reconcile_ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    const bool write_needed = s_persistence_dirty;
    const bool cleanup_needed = s_legacy_cleanup_pending;
    const uint64_t snapshot_revision = s_revision;
    if (write_needed) {
        fill_sd_blob(&s_persist_snapshot);
    }
    d1l_store_lock_give(&s_store_lock);

    esp_err_t ret = ESP_OK;
    if (write_needed) {
        ret = d1l_retained_blob_store_write_sd_primary_guarded(
            D1L_NODE_STORE_ID, D1L_NODE_STORE_SD_KEY,
            &s_persist_snapshot, sizeof(s_persist_snapshot),
            backend.generation);
        if (ret == ESP_OK &&
            !node_sd_backend_generation_matches(backend.generation)) {
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    if (ret == ESP_OK && (write_needed || cleanup_needed)) {
        ret = erase_legacy_node_blob();
    }

    d1l_store_lock_take(&s_store_lock);
    const bool same_revision = s_revision == snapshot_revision;
    if (ret == ESP_OK) {
        if (write_needed) {
            s_persistence_commit_count++;
        }
        s_legacy_cleanup_pending = false;
        s_sd_primary_last_error = ESP_OK;
        s_retry_pending = false;
        if (write_needed && same_revision) {
            s_persistence_dirty = false;
            s_dirty_timing_started = false;
            s_dirty_since_ms = 0U;
        } else if (write_needed) {
            s_persistence_coalesced_count++;
        }
    } else if (ret != ESP_ERR_NOT_FINISHED) {
        s_persistence_fail_count++;
        s_sd_primary_last_error = ret;
        s_retry_pending = true;
        if (write_needed) {
            s_persistence_dirty = true;
        }
        if (cleanup_needed) {
            s_legacy_cleanup_pending = true;
        }
    }
    d1l_store_lock_give(&s_store_lock);
    d1l_store_lock_give(&s_persist_io_lock);
    return ret;
}

esp_err_t d1l_node_store_flush(void)
{
    return persist_node_snapshot(true);
}

esp_err_t d1l_node_store_flush_if_due(void)
{
    return persist_node_snapshot(false);
}

esp_err_t d1l_node_store_upsert_advert(const char *fingerprint, const char *public_key_hex,
                                       const char *name, char type_code, int rssi_dbm,
                                       int snr_tenths, uint8_t path_hash_bytes,
                                       uint8_t path_hops, uint32_t advert_timestamp,
                                       bool location_valid, int32_t lat_e6, int32_t lon_e6,
                                       bool *out_stale)
{
    if (!out_stale || !fingerprint || fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    *out_stale = false;
    if (location_valid && !location_in_bounds(lat_e6, lon_e6)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        esp_err_t ret = d1l_node_store_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    d1l_store_lock_take(&s_store_lock);
    int existing = find_by_fingerprint(fingerprint);
    if (existing >= 0 && public_key_hex && public_key_hex[0] != '\0' &&
        s_entries[existing].public_key_hex[0] != '\0' &&
        !public_keys_equal(s_entries[existing].public_key_hex, public_key_hex)) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (existing >= 0 &&
        !d1l_meshcore_lifetime_advert_is_strictly_newer(
            true, s_entries[existing].advert_timestamp, advert_timestamp)) {
        d1l_store_lock_give(&s_store_lock);
        *out_stale = true;
        return ESP_OK;
    }
    size_t index;
    bool is_new = existing < 0;
    bool replacing_oldest = false;
    if (!is_new) {
        index = (size_t)existing;
    } else if (s_count < D1L_NODE_STORE_CAPACITY) {
        index = s_count++;
    } else {
        /*
         * Located nodes are durable Map state. Never evict one merely because
         * another fingerprint was heard; fail closed when all slots are Map
         * markers and allow existing fingerprints to keep updating in place.
         */
        if (!oldest_unlocated_index(&index)) {
            d1l_store_lock_give(&s_store_lock);
            return ESP_ERR_NO_MEM;
        }
        replacing_oldest = true;
        s_dropped_oldest++;
    }

    d1l_node_entry_t *entry = &s_entries[index];
    const d1l_node_entry_t entry_before = *entry;
    const d1l_node_entry_t marker_before =
        (!is_new || replacing_oldest) ? entry_before : (d1l_node_entry_t){0};
    const uint32_t now_ms = monotonic_ms();
    if (is_new) {
        memset(entry, 0, sizeof(*entry));
        entry->first_heard_ms = now_ms;
        entry->heard_count = 0;
    }
    entry->seq = s_next_seq++;
    entry->last_heard_ms = now_ms;
    s_live_last_heard_ms[index] = now_ms;
    s_live_heard_valid[index] = true;
    entry->advert_timestamp = advert_timestamp;
    entry->heard_count++;
    entry->rssi_dbm = rssi_dbm;
    entry->snr_tenths = snr_tenths;
    entry->path_hash_bytes = path_hash_bytes;
    entry->path_hops = path_hops;
    sanitize_ascii(entry->fingerprint, sizeof(entry->fingerprint), fingerprint);
    if (public_key_hex && public_key_hex[0] != '\0') {
        sanitize_ascii(entry->public_key_hex, sizeof(entry->public_key_hex), public_key_hex);
    }
    if (name && name[0] != '\0') {
        sanitize_ascii(entry->name, sizeof(entry->name), name);
    }
    sanitize_ascii(entry->type, sizeof(entry->type), type_name(type_code));
    if (entry->name[0] == '\0') {
        sanitize_ascii(entry->name, sizeof(entry->name), entry->fingerprint);
    }
    if (location_valid) {
        entry->location_valid = true;
        entry->lat_e6 = lat_e6;
        entry->lon_e6 = lon_e6;
        entry->location_advert_timestamp = advert_timestamp;
        entry->location_seq = entry->seq;
    }
    if (marker_material_changed(&marker_before, entry)) {
        bump_marker_generation();
    }
    s_total_written++;
    s_revision++;
    note_persistence_dirty_locked(false, now_ms);
    d1l_store_lock_give(&s_store_lock);
    return ESP_OK;
}

d1l_node_store_stats_t d1l_node_store_stats(void)
{
    d1l_store_lock_take(&s_store_lock);
    d1l_node_store_stats_t stats = {
        .next_seq = s_next_seq,
        .total_written = s_total_written,
        .dropped_oldest = s_dropped_oldest,
        .persistence_commit_count = s_persistence_commit_count,
        .persistence_coalesced_count = s_persistence_coalesced_count,
        .persistence_fail_count = s_persistence_fail_count,
        .sd_backend_generation = s_last_sd_backend_generation,
        .sd_primary_last_error = s_sd_primary_last_error,
        .persistence_revision = s_revision,
        .count = s_count,
        .capacity = D1L_NODE_STORE_CAPACITY,
        .persistence_dirty =
            s_persistence_dirty || s_legacy_cleanup_pending,
        .sd_primary_reconcile_pending = s_sd_reconcile_pending,
    };
    d1l_store_lock_give(&s_store_lock);
    return stats;
}

bool d1l_node_store_find_by_fingerprint(const char *fingerprint, d1l_node_entry_t *out_entry)
{
    if (!s_loaded && d1l_node_store_init() != ESP_OK) {
        return false;
    }
    d1l_store_lock_take(&s_store_lock);
    int index = find_by_fingerprint(fingerprint);
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

size_t d1l_node_store_copy_recent(d1l_node_entry_t *out_entries, size_t max_entries)
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
    bool used[D1L_NODE_STORE_CAPACITY] = {0};
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

size_t d1l_node_store_query(const d1l_node_query_t *query, d1l_node_view_t *out_entries,
                            size_t max_entries)
{
    if (!out_entries || max_entries == 0) {
        return 0;
    }
    if (!s_loaded && d1l_node_store_init() != ESP_OK) {
        return 0;
    }
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    d1l_store_lock_take(&s_store_lock);
    for (size_t i = 0; i < s_count; ++i) {
        build_node_view(i, &s_entries[i], &s_query_scratch[i], now_ms);
    }

    const d1l_node_sort_t sort = query ? query->sort : D1L_NODE_SORT_LAST_HEARD;
    bool used[D1L_NODE_STORE_CAPACITY] = {0};
    size_t copied = 0;
    while (copied < max_entries) {
        size_t best = 0;
        bool best_set = false;
        for (size_t i = 0; i < s_count; ++i) {
            if (used[i] || !node_view_matches_query(&s_query_scratch[i], query)) {
                continue;
            }
            if (!best_set || node_view_better(&s_query_scratch[i], &s_query_scratch[best], sort)) {
                best = i;
                best_set = true;
            }
        }
        if (!best_set) {
            break;
        }
        used[best] = true;
        out_entries[copied++] = s_query_scratch[best];
    }
    d1l_store_lock_give(&s_store_lock);
    return copied;
}

uint32_t d1l_node_store_marker_generation(void)
{
    if (!s_loaded && d1l_node_store_init() != ESP_OK) {
        return 0U;
    }
    d1l_store_lock_take(&s_store_lock);
    const uint32_t generation = s_marker_generation;
    d1l_store_lock_give(&s_store_lock);
    return generation;
}

size_t d1l_node_store_copy_markers(d1l_node_marker_t *out_markers, size_t max_markers)
{
    if (!out_markers || max_markers == 0U) {
        return 0U;
    }
    if (!s_loaded && d1l_node_store_init() != ESP_OK) {
        return 0U;
    }

    d1l_store_lock_take(&s_store_lock);
    bool used[D1L_NODE_STORE_CAPACITY] = {0};
    size_t copied = 0U;
    while (copied < max_markers) {
        size_t best = 0U;
        bool best_set = false;
        for (size_t i = 0; i < s_count; ++i) {
            if (used[i] || !s_entries[i].location_valid) {
                continue;
            }
            if (!best_set || s_entries[i].location_seq >
                                 s_entries[best].location_seq ||
                (s_entries[i].location_seq == s_entries[best].location_seq &&
                 s_entries[i].seq > s_entries[best].seq)) {
                best = i;
                best_set = true;
            }
        }
        if (!best_set) {
            break;
        }
        used[best] = true;
        d1l_node_marker_t *marker = &out_markers[copied++];
        memset(marker, 0, sizeof(*marker));
        sanitize_ascii(marker->fingerprint, sizeof(marker->fingerprint),
                       s_entries[best].fingerprint);
        sanitize_ascii(marker->name, sizeof(marker->name), s_entries[best].name);
        sanitize_ascii(marker->type, sizeof(marker->type), s_entries[best].type);
        marker->lat_e6 = s_entries[best].lat_e6;
        marker->lon_e6 = s_entries[best].lon_e6;
        marker->location_advert_timestamp = s_entries[best].location_advert_timestamp;
        marker->location_seq = s_entries[best].location_seq;
        marker->location_provenance =
            D1L_NODE_LOCATION_PROVENANCE_SIGNED_ADVERT;
    }
    d1l_store_lock_give(&s_store_lock);
    return copied;
}
