#include "factory_reset.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#include "nvs.h"

#define D1L_FACTORY_RESET_JOURNAL_NAMESPACE "d1l_reset"
#define D1L_FACTORY_RESET_JOURNAL_KEY "factory_v1"
#define D1L_FACTORY_RESET_RETAINED_PARTITION "d1l_retained"
#define D1L_FACTORY_RESET_JOURNAL_MAGIC UINT32_C(0x31524644)
#define D1L_FACTORY_RESET_JOURNAL_SCHEMA 2U
#define D1L_FACTORY_RESET_JOURNAL_FLAG_EXPLICIT_REPAIR UINT32_C(1)
#define D1L_FACTORY_RESET_SD_LINEAGE_MAGIC UINT32_C(0x314c5344)
#define D1L_FACTORY_RESET_SD_LINEAGE_SCHEMA 1U
#define D1L_FACTORY_RESET_SD_MEDIA_MARKER_MAGIC UINT32_C(0x314d5344)

typedef enum {
    D1L_FACTORY_RESET_JOURNAL_ACTIVE = 1,
    D1L_FACTORY_RESET_JOURNAL_COMPLETE = 2,
} d1l_factory_reset_journal_phase_t;

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t phase;
    uint32_t attempt_count;
    uint32_t completed_domains;
    uint32_t next_domain;
    uint32_t last_failed_domain;
    int32_t last_error;
    uint32_t erased_count;
    uint32_t absent_count;
    uint32_t sd_lineage_generation;
    uint32_t flags;
    uint32_t checksum;
} d1l_factory_reset_journal_t;

_Static_assert(sizeof(d1l_factory_reset_journal_t) == 48U,
               "factory reset journal layout must remain stable");

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t store;
    uint32_t generation;
    uint32_t active;
    uint32_t reserved;
    uint32_t checksum;
} d1l_factory_reset_sd_lineage_t;

_Static_assert(sizeof(d1l_factory_reset_sd_lineage_t) == 24U,
               "factory reset SD lineage layout must remain stable");

static const char *const s_sd_lineage_keys[D1L_FACTORY_RESET_SD_STORE_COUNT] = {
    "sd_public_v1",
    "sd_dm_v1",
    "sd_routes_v1",
    "sd_packets_v1",
    "sd_nodes_v1",
    "sd_contacts_v1",
    "sd_read_v1",
};

typedef struct {
    atomic_bool update_locked;
    _Atomic(uint32_t) sequence;
    atomic_bool loaded;
    atomic_bool active;
    _Atomic(uint32_t) generation;
} d1l_factory_reset_sd_lineage_cache_t;

static d1l_factory_reset_sd_lineage_cache_t
    s_sd_lineage_cache[D1L_FACTORY_RESET_SD_STORE_COUNT];

_Static_assert(sizeof(d1l_factory_reset_sd_media_marker_t) == 24U,
               "factory reset SD media marker layout must remain stable");

/* Clear entries are deliberately first. Early-boot resume iterates exactly the
 * first D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT entries; preserved evidence is
 * still inventoried so new retained keys cannot be added invisibly. */
static const d1l_factory_reset_inventory_entry_t s_inventory[] = {
    {"settings", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_settings", "settings", D1L_FACTORY_RESET_DISPOSITION_CLEAR, true,
     "user settings, Wi-Fi credential, and identity key material",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"connectivity_guard", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_conn", "boot_guard", D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "Wi-Fi crash attribution belongs to the cleared profile",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"channels", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_channels", "channels", D1L_FACTORY_RESET_DISPOSITION_CLEAR, true,
     "channel configuration and channel secrets",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"contacts", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_contacts", "contacts", D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "user contact book", D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"heard_nodes", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_nodes", "heard", D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "legacy device-local heard-node history",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"heard_nodes_epoch", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_nodes", "marker_epoch", D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "small anti-resurrection marker for explicit node-list clear",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"read_state", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_read", "state", D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "message read cursors tied to cleared histories",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"public_messages_retained", D1L_FACTORY_RESET_PARTITION_RETAINED,
     D1L_FACTORY_RESET_RETAINED_PARTITION, "d1l_messages", "public",
     D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "dedicated-NVS Public/channel history",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"public_messages_legacy", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_messages", "public", D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "legacy default-NVS Public/channel mirror",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"direct_messages_retained", D1L_FACTORY_RESET_PARTITION_RETAINED,
     D1L_FACTORY_RESET_RETAINED_PARTITION, "d1l_dms", "threads",
     D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "dedicated-NVS direct-message history and delivery state",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"direct_messages_legacy", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_dms", "threads", D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "legacy default-NVS direct-message mirror",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"routes_v2_retained", D1L_FACTORY_RESET_PARTITION_RETAINED,
     D1L_FACTORY_RESET_RETAINED_PARTITION, "d1l_routes", "routes_v2",
     D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "dedicated-NVS current route history",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"routes_legacy_retained", D1L_FACTORY_RESET_PARTITION_RETAINED,
     D1L_FACTORY_RESET_RETAINED_PARTITION, "d1l_routes", "routes",
     D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "dedicated-NVS legacy route history",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"routes_v2_legacy", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_routes", "routes_v2", D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "legacy default-NVS current route mirror",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"routes_v1_legacy", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_routes", "routes", D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "legacy default-NVS route mirror",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"packet_log_retained", D1L_FACTORY_RESET_PARTITION_RETAINED,
     D1L_FACTORY_RESET_RETAINED_PARTITION, "d1l_packets", "ring",
     D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "dedicated-NVS packet-log fallback",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"packet_log_legacy", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_packets", "ring", D1L_FACTORY_RESET_DISPOSITION_CLEAR, false,
     "legacy default-NVS packet-log mirror",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"admin_credentials", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_admin_pw", "credentials", D1L_FACTORY_RESET_DISPOSITION_CLEAR,
     true, "device-local repeater and room login passwords",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},

    {"legacy_protocol_timestamp", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_settings", "mesh_ts",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_MIGRATION_EVIDENCE, false,
     "unconfirmed legacy timestamp remains fail-closed migration evidence",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"protocol_high_water", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_settings", "mesh_hi_v2",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_PROTOCOL_GUARD, false,
     "monotonic protocol anti-rollback high-water",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"protocol_migration_receipt", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_time_mig", "mesh_mig_v1",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_MIGRATION_EVIDENCE, false,
     "checksummed migration receipt and quarantine evidence",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"wall_checkpoint", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_time", "wall_ckpt_v1",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_TIME_CHECKPOINT, false,
     "validated wall checkpoint cannot weaken protocol rollback protection",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"crash_forensics", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     "d1l_crash", "ring",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_CRASH_FORENSICS, false,
     "reset and failure evidence remains available for recovery diagnosis",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"retained_completion_sentinel", D1L_FACTORY_RESET_PARTITION_DEFAULT,
     "nvs", "d1l_ret_meta", "initialized",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE, false,
     "default-NVS retained-partition ownership sentinel",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"public_history_sd_only", D1L_FACTORY_RESET_PARTITION_DEFAULT,
     "nvs", "d1l_ret_meta", "sd_pub_v1",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE, false,
     "prevents retired Public history from resurrecting after SD migration",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"dm_history_sd_only", D1L_FACTORY_RESET_PARTITION_DEFAULT,
     "nvs", "d1l_ret_meta", "sd_dm_v1",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE, false,
     "prevents retired direct-message history from resurrecting after SD migration",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"route_history_sd_only", D1L_FACTORY_RESET_PARTITION_DEFAULT,
     "nvs", "d1l_ret_meta", "sd_route_v1",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE, false,
     "prevents retired route history from resurrecting after SD migration",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"packet_history_sd_only", D1L_FACTORY_RESET_PARTITION_DEFAULT,
     "nvs", "d1l_ret_meta", "sd_pkt_v1",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE, false,
     "prevents retired packet history from resurrecting after SD migration",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"contacts_sd_only", D1L_FACTORY_RESET_PARTITION_DEFAULT,
     "nvs", "d1l_ret_meta", "sd_contacts_v1",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE, false,
     "prevents retired contact data from resurrecting after SD migration",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"read_state_sd_only", D1L_FACTORY_RESET_PARTITION_DEFAULT,
     "nvs", "d1l_ret_meta", "sd_read_v1",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE, false,
     "prevents retired read cursors from resurrecting after SD migration",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"retained_partition_anchor", D1L_FACTORY_RESET_PARTITION_RETAINED,
     D1L_FACTORY_RESET_RETAINED_PARTITION, "d1l_ret_meta", "anchor",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE, false,
     "dedicated-NVS ownership anchor",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"retained_marker_first", D1L_FACTORY_RESET_PARTITION_RETAINED_META,
     "d1l_ret_meta", "", "",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE, false,
     "raw retained ownership marker at the metadata partition start",
     D1L_FACTORY_RESET_RAW_SLOT_PARTITION_START,
     D1L_FACTORY_RESET_RAW_MARKER_BYTES},
    {"retained_marker_last", D1L_FACTORY_RESET_PARTITION_RETAINED_META,
     "d1l_ret_meta", "", "",
     D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE, false,
     "raw retained ownership marker ending at the metadata partition end",
     D1L_FACTORY_RESET_RAW_SLOT_PARTITION_END,
     D1L_FACTORY_RESET_RAW_MARKER_BYTES},
    {"factory_reset_journal", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     D1L_FACTORY_RESET_JOURNAL_NAMESPACE, D1L_FACTORY_RESET_JOURNAL_KEY,
     D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL, false,
     "interruption-safe reset progress; removed after completed reboot",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"sd_public_lineage", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     D1L_FACTORY_RESET_JOURNAL_NAMESPACE, "sd_public_v1",
     D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL, false,
     "durable post-reset public-message removable-SD lineage fence",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"sd_dm_lineage", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     D1L_FACTORY_RESET_JOURNAL_NAMESPACE, "sd_dm_v1",
     D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL, false,
     "durable post-reset direct-message removable-SD lineage fence",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"sd_route_lineage", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     D1L_FACTORY_RESET_JOURNAL_NAMESPACE, "sd_routes_v1",
     D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL, false,
     "durable post-reset route removable-SD lineage fence",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"sd_packet_lineage", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     D1L_FACTORY_RESET_JOURNAL_NAMESPACE, "sd_packets_v1",
     D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL, false,
     "durable post-reset packet primary and segment lineage fence",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"sd_node_lineage", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     D1L_FACTORY_RESET_JOURNAL_NAMESPACE, "sd_nodes_v1",
     D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL, false,
     "durable post-reset node-history removable-SD lineage fence",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"sd_contact_lineage", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     D1L_FACTORY_RESET_JOURNAL_NAMESPACE, "sd_contacts_v1",
     D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL, false,
     "durable post-reset contact-book removable-SD lineage fence",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
    {"sd_read_state_lineage", D1L_FACTORY_RESET_PARTITION_DEFAULT, "nvs",
     D1L_FACTORY_RESET_JOURNAL_NAMESPACE, "sd_read_v1",
     D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL, false,
     "durable post-reset read-cursor removable-SD lineage fence",
     D1L_FACTORY_RESET_RAW_SLOT_NONE, 0U},
};

_Static_assert(sizeof(s_inventory) / sizeof(s_inventory[0]) ==
                   D1L_FACTORY_RESET_INVENTORY_COUNT,
               "factory reset inventory count changed");

typedef enum {
    JOURNAL_LOAD_ABSENT = 0,
    JOURNAL_LOAD_ACTIVE,
    JOURNAL_LOAD_COMPLETE,
    JOURNAL_LOAD_NEWER,
    JOURNAL_LOAD_CORRUPT,
    JOURNAL_LOAD_ERROR,
} journal_load_state_t;

static uint32_t crc32_ieee(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= bytes[i];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static void sd_lineage_cache_lock(
    d1l_factory_reset_sd_lineage_cache_t *cache)
{
    bool expected = false;
    while (!atomic_compare_exchange_weak_explicit(
        &cache->update_locked, &expected, true, memory_order_acquire,
        memory_order_relaxed)) {
        expected = false;
    }
}

static bool sd_lineage_cache_try_lock(
    d1l_factory_reset_sd_lineage_cache_t *cache)
{
    bool expected = false;
    return atomic_compare_exchange_strong_explicit(
        &cache->update_locked, &expected, true, memory_order_acquire,
        memory_order_relaxed);
}

static void sd_lineage_cache_unlock(
    d1l_factory_reset_sd_lineage_cache_t *cache)
{
    atomic_store_explicit(&cache->update_locked, false,
                          memory_order_release);
}

static void sd_lineage_cache_publish(
    d1l_factory_reset_sd_lineage_cache_t *cache, bool loaded, bool active,
    uint32_t generation)
{
    (void)atomic_fetch_add_explicit(&cache->sequence, 1U,
                                    memory_order_acq_rel);
    atomic_store_explicit(&cache->loaded, loaded, memory_order_relaxed);
    atomic_store_explicit(&cache->active, active, memory_order_relaxed);
    atomic_store_explicit(&cache->generation, generation,
                          memory_order_relaxed);
    (void)atomic_fetch_add_explicit(&cache->sequence, 1U,
                                    memory_order_release);
}

static void sd_lineage_cache_read(
    const d1l_factory_reset_sd_lineage_cache_t *cache, bool *out_loaded,
    bool *out_active, uint32_t *out_generation)
{
    for (;;) {
        const uint32_t before = atomic_load_explicit(
            &cache->sequence, memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        const bool loaded = atomic_load_explicit(
            &cache->loaded, memory_order_relaxed);
        const bool active = atomic_load_explicit(
            &cache->active, memory_order_relaxed);
        const uint32_t generation = atomic_load_explicit(
            &cache->generation, memory_order_relaxed);
        const uint32_t after = atomic_load_explicit(
            &cache->sequence, memory_order_acquire);
        if (before == after && (after & 1U) == 0U) {
            *out_loaded = loaded;
            *out_active = active;
            *out_generation = generation;
            return;
        }
    }
}

static void sd_lineage_cache_reset(void)
{
    for (uint32_t i = 0U; i < D1L_FACTORY_RESET_SD_STORE_COUNT; ++i) {
        d1l_factory_reset_sd_lineage_cache_t *cache =
            &s_sd_lineage_cache[i];
        sd_lineage_cache_lock(cache);
        sd_lineage_cache_publish(cache, false, false, 0U);
        sd_lineage_cache_unlock(cache);
    }
}

static uint32_t sd_media_marker_checksum(
    const d1l_factory_reset_sd_media_marker_t *marker)
{
    return marker ?
        crc32_ieee(marker,
                   offsetof(d1l_factory_reset_sd_media_marker_t, checksum)) :
        0U;
}

void d1l_factory_reset_sd_media_marker_init(
    d1l_factory_reset_sd_media_marker_t *marker,
    d1l_factory_reset_sd_store_t store, uint32_t generation)
{
    if (!marker) {
        return;
    }
    memset(marker, 0, sizeof(*marker));
    if ((uint32_t)store >= D1L_FACTORY_RESET_SD_STORE_COUNT ||
        generation == 0U) {
        return;
    }
    marker->magic = D1L_FACTORY_RESET_SD_MEDIA_MARKER_MAGIC;
    marker->schema = D1L_FACTORY_RESET_SD_MEDIA_MARKER_SCHEMA;
    marker->store = (uint16_t)store;
    marker->generation = generation;
    marker->generation_inverse = ~generation;
    marker->checksum = sd_media_marker_checksum(marker);
}

bool d1l_factory_reset_sd_media_marker_matches(
    const d1l_factory_reset_sd_media_marker_t *marker, size_t marker_length,
    d1l_factory_reset_sd_store_t store, uint32_t generation)
{
    return marker && marker_length == sizeof(*marker) &&
        (uint32_t)store < D1L_FACTORY_RESET_SD_STORE_COUNT &&
        generation != 0U &&
        marker->magic == D1L_FACTORY_RESET_SD_MEDIA_MARKER_MAGIC &&
        marker->schema == D1L_FACTORY_RESET_SD_MEDIA_MARKER_SCHEMA &&
        marker->store == (uint16_t)store &&
        marker->generation == generation &&
        marker->generation_inverse == ~generation &&
        marker->reserved == 0U &&
        marker->checksum == sd_media_marker_checksum(marker);
}

static uint32_t sd_lineage_checksum(
    const d1l_factory_reset_sd_lineage_t *lineage)
{
    return lineage ?
        crc32_ieee(lineage,
                   offsetof(d1l_factory_reset_sd_lineage_t, checksum)) :
        0U;
}

static bool sd_lineage_structurally_valid(
    const d1l_factory_reset_sd_lineage_t *lineage,
    d1l_factory_reset_sd_store_t store)
{
    return lineage && lineage->magic == D1L_FACTORY_RESET_SD_LINEAGE_MAGIC &&
        lineage->schema == D1L_FACTORY_RESET_SD_LINEAGE_SCHEMA &&
        lineage->store == (uint16_t)store && lineage->generation != 0U &&
        lineage->active <= 1U && lineage->reserved == 0U &&
        lineage->checksum == sd_lineage_checksum(lineage);
}

static esp_err_t sd_lineage_load_uncached(
    d1l_factory_reset_sd_store_t store, bool *out_active,
    uint32_t *out_generation)
{
    if ((uint32_t)store >= D1L_FACTORY_RESET_SD_STORE_COUNT ||
        !out_active || !out_generation) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_active = true;
    *out_generation = 0U;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_FACTORY_RESET_JOURNAL_NAMESPACE,
                             NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *out_active = false;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    size_t length = 0U;
    ret = nvs_get_blob(handle, s_sd_lineage_keys[store], NULL, &length);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        *out_active = false;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }
    if (length > sizeof(d1l_factory_reset_sd_lineage_t)) {
        nvs_close(handle);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (length != sizeof(d1l_factory_reset_sd_lineage_t)) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }
    d1l_factory_reset_sd_lineage_t lineage = {0};
    size_t read_length = sizeof(lineage);
    ret = nvs_get_blob(handle, s_sd_lineage_keys[store], &lineage,
                       &read_length);
    nvs_close(handle);
    if (ret != ESP_OK || read_length != sizeof(lineage)) {
        return ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret;
    }
    if (lineage.schema > D1L_FACTORY_RESET_SD_LINEAGE_SCHEMA) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!sd_lineage_structurally_valid(&lineage, store)) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_active = lineage.active != 0U;
    *out_generation = lineage.generation;
    return ESP_OK;
}

esp_err_t d1l_factory_reset_sd_lineage_snapshot(
    d1l_factory_reset_sd_store_t store, bool *out_active,
    uint32_t *out_generation)
{
    if ((uint32_t)store >= D1L_FACTORY_RESET_SD_STORE_COUNT ||
        !out_active || !out_generation) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_factory_reset_sd_lineage_cache_t *cache =
        &s_sd_lineage_cache[store];
    bool loaded = false;
    bool active = true;
    uint32_t generation = 0U;
    sd_lineage_cache_read(cache, &loaded, &active, &generation);
    if (loaded) {
        *out_active = active;
        *out_generation = generation;
        return ESP_OK;
    }

    if (!sd_lineage_cache_try_lock(cache)) {
        *out_active = true;
        *out_generation = generation;
        return ESP_ERR_NOT_FINISHED;
    }
    sd_lineage_cache_read(cache, &loaded, &active, &generation);
    esp_err_t ret = ESP_OK;
    if (!loaded) {
        ret = sd_lineage_load_uncached(store, &active, &generation);
        /* Storage errors are fail-closed for this operation but are not
         * sticky; a later recovery/status transaction re-reads NVS. */
        sd_lineage_cache_publish(cache, ret == ESP_OK, active, generation);
    }
    sd_lineage_cache_unlock(cache);
    *out_active = ret == ESP_OK ? active : true;
    *out_generation = generation;
    return ret;
}

static esp_err_t sd_lineage_store(d1l_factory_reset_sd_store_t store,
                                  uint32_t generation, bool active)
{
    if ((uint32_t)store >= D1L_FACTORY_RESET_SD_STORE_COUNT ||
        generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_factory_reset_sd_lineage_t lineage = {
        .magic = D1L_FACTORY_RESET_SD_LINEAGE_MAGIC,
        .schema = D1L_FACTORY_RESET_SD_LINEAGE_SCHEMA,
        .store = (uint16_t)store,
        .generation = generation,
        .active = active ? 1U : 0U,
    };
    lineage.checksum = sd_lineage_checksum(&lineage);
    d1l_factory_reset_sd_lineage_cache_t *cache =
        &s_sd_lineage_cache[store];
    sd_lineage_cache_lock(cache);
    sd_lineage_cache_publish(cache, false, true, generation);

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_FACTORY_RESET_JOURNAL_NAMESPACE,
                             NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        sd_lineage_cache_unlock(cache);
        return ret;
    }
    ret = nvs_set_blob(handle, s_sd_lineage_keys[store], &lineage,
                       sizeof(lineage));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret == ESP_OK) {
        sd_lineage_cache_publish(cache, true, active, generation);
    }
    sd_lineage_cache_unlock(cache);
    return ret;
}

static esp_err_t next_sd_lineage_generation(bool tolerate_invalid,
                                            uint32_t *out_generation)
{
    if (!out_generation) {
        return ESP_ERR_INVALID_ARG;
    }
    sd_lineage_cache_reset();
    uint32_t maximum = 0U;
    for (uint32_t i = 0U; i < D1L_FACTORY_RESET_SD_STORE_COUNT; ++i) {
        bool active = false;
        uint32_t generation = 0U;
        const esp_err_t ret = d1l_factory_reset_sd_lineage_snapshot(
            (d1l_factory_reset_sd_store_t)i, &active, &generation);
        (void)active;
        if (ret != ESP_OK) {
            if (tolerate_invalid) {
                continue;
            }
            return ret;
        }
        if (generation > maximum) {
            maximum = generation;
        }
    }
    if (maximum == UINT32_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_generation = maximum + 1U;
    return ESP_OK;
}

static esp_err_t arm_sd_lineage(uint32_t generation)
{
    if (generation == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    for (uint32_t i = 0U; i < D1L_FACTORY_RESET_SD_STORE_COUNT; ++i) {
        bool active = false;
        uint32_t current_generation = 0U;
        const esp_err_t read_ret = d1l_factory_reset_sd_lineage_snapshot(
            (d1l_factory_reset_sd_store_t)i, &active,
            &current_generation);
        if (read_ret == ESP_OK && active &&
            current_generation == generation) {
            continue;
        }
        const esp_err_t write_ret = sd_lineage_store(
            (d1l_factory_reset_sd_store_t)i, generation, true);
        if (write_ret != ESP_OK) {
            return write_ret;
        }
    }
    return ESP_OK;
}

esp_err_t d1l_factory_reset_sd_lineage_clear(
    d1l_factory_reset_sd_store_t store, uint32_t expected_generation)
{
    if ((uint32_t)store >= D1L_FACTORY_RESET_SD_STORE_COUNT ||
        expected_generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    bool active = false;
    uint32_t generation = 0U;
    const esp_err_t ret = d1l_factory_reset_sd_lineage_snapshot(
        store, &active, &generation);
    if (ret != ESP_OK) {
        return ret;
    }
    if (generation != expected_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    return active ? sd_lineage_store(store, generation, false) : ESP_OK;
}

static uint32_t journal_checksum(
    const d1l_factory_reset_journal_t *journal)
{
    return journal ?
        crc32_ieee(journal, offsetof(d1l_factory_reset_journal_t, checksum)) :
        0U;
}

static d1l_factory_reset_status_t status_defaults(void)
{
    d1l_factory_reset_status_t status = {
        .phase = D1L_FACTORY_RESET_PHASE_IDLE,
        .domains_total = D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT,
        .last_failed_domain_index = UINT32_MAX,
        .last_error = ESP_OK,
        .global_atomic = false,
        .physical_flash_scrubbed = false,
        .sd_touched = false,
        .removable_sd_data_preserved = true,
        .unknown_keys_preserved = true,
        .protocol_high_water_preserved = true,
        .migration_evidence_preserved = true,
        .time_checkpoint_preserved = true,
        .crash_forensics_preserved = true,
        .retained_ownership_evidence_preserved = true,
    };
    return status;
}

static void copy_failed_domain(d1l_factory_reset_status_t *status,
                               uint32_t index)
{
    if (!status) {
        return;
    }
    status->last_failed_domain[0] = '\0';
    if (index < D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT) {
        (void)strncpy(status->last_failed_domain, s_inventory[index].name,
                      sizeof(status->last_failed_domain) - 1U);
    }
}

static void status_from_journal(
    const d1l_factory_reset_journal_t *journal,
    d1l_factory_reset_status_t *status)
{
    if (!journal || !status) {
        return;
    }
    status->journal_found = true;
    status->phase = journal->phase == D1L_FACTORY_RESET_JOURNAL_COMPLETE ?
        D1L_FACTORY_RESET_PHASE_COMPLETE : D1L_FACTORY_RESET_PHASE_ACTIVE;
    status->reset_pending =
        journal->phase == D1L_FACTORY_RESET_JOURNAL_ACTIVE;
    status->reset_complete =
        journal->phase == D1L_FACTORY_RESET_JOURNAL_COMPLETE;
    status->attempt_count = journal->attempt_count;
    status->domains_completed = journal->completed_domains;
    status->keys_erased = journal->erased_count;
    status->keys_already_absent = journal->absent_count;
    status->sd_lineage_generation = journal->sd_lineage_generation;
    status->next_domain_index = journal->next_domain;
    status->last_failed_domain_index = journal->last_failed_domain;
    status->last_error = (esp_err_t)journal->last_error;
    copy_failed_domain(status, journal->last_failed_domain);
}

static void populate_sd_lineage_status(d1l_factory_reset_status_t *status)
{
    if (!status) {
        return;
    }
    status->sd_lineage_active_mask = 0U;
    for (uint32_t i = 0U; i < D1L_FACTORY_RESET_SD_STORE_COUNT; ++i) {
        bool active = true;
        uint32_t generation = 0U;
        const esp_err_t ret = d1l_factory_reset_sd_lineage_snapshot(
            (d1l_factory_reset_sd_store_t)i, &active, &generation);
        if (ret != ESP_OK || active) {
            status->sd_lineage_active_mask |= UINT32_C(1) << i;
        }
        if (generation > status->sd_lineage_generation) {
            status->sd_lineage_generation = generation;
        }
    }
}

static bool journal_structurally_valid(
    const d1l_factory_reset_journal_t *journal)
{
    if (!journal || journal->magic != D1L_FACTORY_RESET_JOURNAL_MAGIC ||
        journal->schema != D1L_FACTORY_RESET_JOURNAL_SCHEMA ||
        journal->checksum != journal_checksum(journal) ||
        (journal->phase != D1L_FACTORY_RESET_JOURNAL_ACTIVE &&
         journal->phase != D1L_FACTORY_RESET_JOURNAL_COMPLETE) ||
        journal->completed_domains > D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT ||
        journal->next_domain > D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT ||
        journal->completed_domains != journal->next_domain ||
        (journal->last_failed_domain != UINT32_MAX &&
         journal->last_failed_domain >=
             D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT) ||
        journal->sd_lineage_generation == 0U ||
        (journal->flags & ~D1L_FACTORY_RESET_JOURNAL_FLAG_EXPLICIT_REPAIR) !=
            0U ||
        (journal->phase == D1L_FACTORY_RESET_JOURNAL_ACTIVE &&
         ((journal->last_error == ESP_OK) !=
          (journal->last_failed_domain == UINT32_MAX)))) {
        return false;
    }
    return journal->phase != D1L_FACTORY_RESET_JOURNAL_COMPLETE ||
        (journal->completed_domains == D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT &&
         journal->next_domain == D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT &&
         journal->last_error == ESP_OK);
}

static journal_load_state_t journal_load(
    d1l_factory_reset_journal_t *out_journal, esp_err_t *out_error)
{
    if (!out_journal || !out_error) {
        if (out_error) {
            *out_error = ESP_ERR_INVALID_ARG;
        }
        return JOURNAL_LOAD_ERROR;
    }
    memset(out_journal, 0, sizeof(*out_journal));
    *out_error = ESP_OK;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_FACTORY_RESET_JOURNAL_NAMESPACE,
                             NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return JOURNAL_LOAD_ABSENT;
    }
    if (ret != ESP_OK) {
        *out_error = ret;
        return JOURNAL_LOAD_ERROR;
    }

    size_t length = 0U;
    ret = nvs_get_blob(handle, D1L_FACTORY_RESET_JOURNAL_KEY, NULL, &length);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return JOURNAL_LOAD_ABSENT;
    }
    if (ret != ESP_OK) {
        nvs_close(handle);
        *out_error = ret;
        return JOURNAL_LOAD_ERROR;
    }
    if (length > sizeof(*out_journal)) {
        nvs_close(handle);
        *out_error = ESP_ERR_NOT_SUPPORTED;
        return JOURNAL_LOAD_NEWER;
    }
    if (length != sizeof(*out_journal)) {
        nvs_close(handle);
        *out_error = ESP_ERR_INVALID_SIZE;
        return JOURNAL_LOAD_CORRUPT;
    }
    size_t read_length = sizeof(*out_journal);
    ret = nvs_get_blob(handle, D1L_FACTORY_RESET_JOURNAL_KEY, out_journal,
                       &read_length);
    nvs_close(handle);
    if (ret != ESP_OK || read_length != sizeof(*out_journal)) {
        *out_error = ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret;
        return JOURNAL_LOAD_ERROR;
    }
    if (out_journal->schema > D1L_FACTORY_RESET_JOURNAL_SCHEMA) {
        *out_error = ESP_ERR_NOT_SUPPORTED;
        return JOURNAL_LOAD_NEWER;
    }
    if (!journal_structurally_valid(out_journal)) {
        *out_error = ESP_ERR_INVALID_STATE;
        return JOURNAL_LOAD_CORRUPT;
    }
    return out_journal->phase == D1L_FACTORY_RESET_JOURNAL_COMPLETE ?
        JOURNAL_LOAD_COMPLETE : JOURNAL_LOAD_ACTIVE;
}

typedef struct {
    bool commit_attempted;
    bool write_may_have_applied;
} journal_store_outcome_t;

static esp_err_t journal_store_with_outcome(
    d1l_factory_reset_journal_t *journal,
    journal_store_outcome_t *out_outcome)
{
    if (!journal) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_outcome) {
        memset(out_outcome, 0, sizeof(*out_outcome));
    }
    journal->checksum = journal_checksum(journal);
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_FACTORY_RESET_JOURNAL_NAMESPACE,
                             NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_blob(handle, D1L_FACTORY_RESET_JOURNAL_KEY, journal,
                       sizeof(*journal));
    if (ret == ESP_OK) {
        if (out_outcome) {
            out_outcome->commit_attempted = true;
            out_outcome->write_may_have_applied = true;
        }
        ret = nvs_commit(handle);
    } else if (ret == ESP_ERR_NVS_REMOVE_FAILED && out_outcome) {
        /* ESP-IDF documents this as fail-after-write: the new value may be
         * readable even though an obsolete entry could not be removed. */
        out_outcome->write_may_have_applied = true;
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t journal_store(d1l_factory_reset_journal_t *journal)
{
    return journal_store_with_outcome(journal, NULL);
}

static esp_err_t journal_erase(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_FACTORY_RESET_JOURNAL_NAMESPACE,
                             NVS_READWRITE, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_erase_key(handle, D1L_FACTORY_RESET_JOURNAL_KEY);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t erase_inventory_key(
    const d1l_factory_reset_inventory_entry_t *entry, bool *out_absent)
{
    if (!entry || entry->disposition != D1L_FACTORY_RESET_DISPOSITION_CLEAR ||
        !out_absent) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_absent = false;
    nvs_handle_t handle;
    esp_err_t ret = entry->partition ==
                            D1L_FACTORY_RESET_PARTITION_RETAINED ?
        nvs_open_from_partition(entry->partition_label, entry->nvs_namespace,
                                NVS_READWRITE, &handle) :
        nvs_open(entry->nvs_namespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_erase_key(handle, entry->key);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *out_absent = true;
        nvs_close(handle);
        return ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static void set_storage_failure(d1l_factory_reset_status_t *status,
                                esp_err_t error, const char *domain)
{
    if (!status) {
        return;
    }
    status->phase = D1L_FACTORY_RESET_PHASE_STORAGE_ERROR;
    status->reset_pending = true;
    status->reset_complete = false;
    status->last_error = error;
    status->last_failed_domain_index = UINT32_MAX;
    status->last_failed_domain[0] = '\0';
    if (domain) {
        (void)strncpy(status->last_failed_domain, domain,
                      sizeof(status->last_failed_domain) - 1U);
    }
}

static esp_err_t process_active_journal(
    d1l_factory_reset_journal_t *journal,
    d1l_factory_reset_status_t *status)
{
    if (!journal || !status) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t lineage_ret = arm_sd_lineage(
        journal->sd_lineage_generation);
    if (lineage_ret != ESP_OK) {
        status_from_journal(journal, status);
        set_storage_failure(status, lineage_ret, "sd_lineage_fence");
        populate_sd_lineage_status(status);
        return lineage_ret;
    }
    populate_sd_lineage_status(status);
    for (uint32_t i = 0U; i < D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT; ++i) {
        bool absent = false;
        const esp_err_t erase_ret = erase_inventory_key(&s_inventory[i],
                                                        &absent);
        if (erase_ret != ESP_OK) {
            journal->phase = D1L_FACTORY_RESET_JOURNAL_ACTIVE;
            journal->completed_domains = i;
            journal->next_domain = i;
            journal->last_failed_domain = i;
            journal->last_error = erase_ret;
            const esp_err_t telemetry_ret = journal_store(journal);
            status_from_journal(journal, status);
            status->failure_telemetry_persisted = telemetry_ret == ESP_OK;
            status->last_error = erase_ret;
            return erase_ret;
        }

        if (absent) {
            journal->absent_count++;
        } else {
            journal->erased_count++;
        }
        journal->completed_domains = i + 1U;
        journal->next_domain = i + 1U;
        journal->last_failed_domain = UINT32_MAX;
        journal->last_error = ESP_OK;
        journal->phase = i + 1U == D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT ?
            D1L_FACTORY_RESET_JOURNAL_COMPLETE :
            D1L_FACTORY_RESET_JOURNAL_ACTIVE;
        const esp_err_t progress_ret = journal_store(journal);
        if (progress_ret != ESP_OK) {
            status_from_journal(journal, status);
            set_storage_failure(status, progress_ret, "reset_journal");
            status->domains_completed = i + 1U;
            status->keys_erased = journal->erased_count;
            status->keys_already_absent = journal->absent_count;
            status->failure_telemetry_persisted = false;
            return progress_ret;
        }
    }
    status_from_journal(journal, status);
    status->failure_telemetry_persisted = true;
    return ESP_OK;
}

static void prepare_retry(d1l_factory_reset_journal_t *journal)
{
    if (!journal) {
        return;
    }
    if (journal->attempt_count < UINT32_MAX) {
        journal->attempt_count++;
    }
    /* Always replay from zero. A producer could have recreated an earlier key
     * after a failed runtime attempt released its quiesce, and an interrupted
     * progress commit may lag a successfully committed key erase. */
    journal->phase = D1L_FACTORY_RESET_JOURNAL_ACTIVE;
    journal->completed_domains = 0U;
    journal->next_domain = 0U;
    journal->last_failed_domain = UINT32_MAX;
    journal->last_error = ESP_OK;
    journal->erased_count = 0U;
    journal->absent_count = 0U;
}

static esp_err_t quarantine_or_error_status(
    journal_load_state_t load_state, esp_err_t load_error,
    d1l_factory_reset_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    status->journal_found = load_state != JOURNAL_LOAD_ABSENT;
    status->last_error = load_error;
    status->phase = load_state == JOURNAL_LOAD_NEWER ?
        D1L_FACTORY_RESET_PHASE_QUARANTINED_NEWER_SCHEMA :
        load_state == JOURNAL_LOAD_CORRUPT ?
            D1L_FACTORY_RESET_PHASE_QUARANTINED_CORRUPT :
            D1L_FACTORY_RESET_PHASE_STORAGE_ERROR;
    status->reset_pending = load_state != JOURNAL_LOAD_ABSENT;
    status->last_failed_domain_index = UINT32_MAX;
    (void)strncpy(status->last_failed_domain, "reset_journal",
                  sizeof(status->last_failed_domain) - 1U);
    return load_error;
}

size_t d1l_factory_reset_inventory_count(void)
{
    return sizeof(s_inventory) / sizeof(s_inventory[0]);
}

bool d1l_factory_reset_inventory_entry(
    size_t index, d1l_factory_reset_inventory_entry_t *out_entry)
{
    if (!out_entry || index >= d1l_factory_reset_inventory_count()) {
        return false;
    }
    *out_entry = s_inventory[index];
    return true;
}

const char *d1l_factory_reset_phase_name(d1l_factory_reset_phase_t phase)
{
    switch (phase) {
    case D1L_FACTORY_RESET_PHASE_IDLE:
        return "idle";
    case D1L_FACTORY_RESET_PHASE_ACTIVE:
        return "active";
    case D1L_FACTORY_RESET_PHASE_COMPLETE:
        return "complete";
    case D1L_FACTORY_RESET_PHASE_QUARANTINED_NEWER_SCHEMA:
        return "quarantined_newer_schema";
    case D1L_FACTORY_RESET_PHASE_QUARANTINED_CORRUPT:
        return "quarantined_corrupt";
    case D1L_FACTORY_RESET_PHASE_STORAGE_ERROR:
    default:
        return "storage_error";
    }
}

const char *d1l_factory_reset_disposition_name(
    d1l_factory_reset_disposition_t disposition)
{
    switch (disposition) {
    case D1L_FACTORY_RESET_DISPOSITION_CLEAR:
        return "clear";
    case D1L_FACTORY_RESET_DISPOSITION_PRESERVE_PROTOCOL_GUARD:
        return "preserve_protocol_guard";
    case D1L_FACTORY_RESET_DISPOSITION_PRESERVE_MIGRATION_EVIDENCE:
        return "preserve_migration_evidence";
    case D1L_FACTORY_RESET_DISPOSITION_PRESERVE_TIME_CHECKPOINT:
        return "preserve_time_checkpoint";
    case D1L_FACTORY_RESET_DISPOSITION_PRESERVE_CRASH_FORENSICS:
        return "preserve_crash_forensics";
    case D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE:
        return "preserve_ownership_evidence";
    case D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL:
    default:
        return "internal_journal";
    }
}

esp_err_t d1l_factory_reset_inspect(d1l_factory_reset_status_t *out_status)
{
    if (!out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_status = status_defaults();
    d1l_factory_reset_journal_t journal = {0};
    esp_err_t load_error = ESP_OK;
    const journal_load_state_t load_state = journal_load(&journal, &load_error);
    if (load_state == JOURNAL_LOAD_ABSENT) {
        populate_sd_lineage_status(out_status);
        return ESP_OK;
    }
    if (load_state == JOURNAL_LOAD_ACTIVE ||
        load_state == JOURNAL_LOAD_COMPLETE) {
        status_from_journal(&journal, out_status);
        populate_sd_lineage_status(out_status);
        return ESP_OK;
    }
    const esp_err_t ret = quarantine_or_error_status(
        load_state, load_error, out_status);
    populate_sd_lineage_status(out_status);
    return ret;
}

esp_err_t d1l_factory_reset_resume(d1l_factory_reset_status_t *out_status)
{
    if (!out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_status = status_defaults();
    sd_lineage_cache_reset();
    d1l_factory_reset_journal_t journal = {0};
    esp_err_t load_error = ESP_OK;
    const journal_load_state_t load_state = journal_load(&journal, &load_error);
    if (load_state == JOURNAL_LOAD_ABSENT) {
        populate_sd_lineage_status(out_status);
        return ESP_OK;
    }
    if (load_state == JOURNAL_LOAD_COMPLETE) {
        status_from_journal(&journal, out_status);
        const esp_err_t erase_ret = journal_erase();
        if (erase_ret != ESP_OK) {
            set_storage_failure(out_status, erase_ret, "reset_journal");
            return erase_ret;
        }
        out_status->journal_found = false;
        out_status->completed_journal_cleaned = true;
        populate_sd_lineage_status(out_status);
        return ESP_OK;
    }
    if (load_state != JOURNAL_LOAD_ACTIVE) {
        const esp_err_t ret = quarantine_or_error_status(
            load_state, load_error, out_status);
        populate_sd_lineage_status(out_status);
        return ret;
    }

    prepare_retry(&journal);
    const esp_err_t checkpoint_ret = journal_store(&journal);
    if (checkpoint_ret != ESP_OK) {
        set_storage_failure(out_status, checkpoint_ret, "reset_journal");
        populate_sd_lineage_status(out_status);
        return checkpoint_ret;
    }
    status_from_journal(&journal, out_status);
    return process_active_journal(&journal, out_status);
}

esp_err_t d1l_factory_reset_request(d1l_factory_reset_status_t *out_status)
{
    if (!out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_status = status_defaults();
    sd_lineage_cache_reset();
    d1l_factory_reset_journal_t journal = {0};
    esp_err_t load_error = ESP_OK;
    const journal_load_state_t load_state = journal_load(&journal, &load_error);
    if (load_state == JOURNAL_LOAD_NEWER ||
        load_state == JOURNAL_LOAD_CORRUPT ||
        load_state == JOURNAL_LOAD_ERROR) {
        const esp_err_t ret = quarantine_or_error_status(
            load_state, load_error, out_status);
        populate_sd_lineage_status(out_status);
        return ret;
    }
    if (load_state == JOURNAL_LOAD_ABSENT ||
        load_state == JOURNAL_LOAD_COMPLETE) {
        memset(&journal, 0, sizeof(journal));
        journal.magic = D1L_FACTORY_RESET_JOURNAL_MAGIC;
        journal.schema = D1L_FACTORY_RESET_JOURNAL_SCHEMA;
        const esp_err_t generation_ret = next_sd_lineage_generation(
            false, &journal.sd_lineage_generation);
        if (generation_ret != ESP_OK) {
            set_storage_failure(out_status, generation_ret,
                                "sd_lineage_fence");
            out_status->reset_pending = false;
            populate_sd_lineage_status(out_status);
            return generation_ret;
        }
    }
    /* A request is only a durable boot intent. Never erase a user domain from
     * a live runtime: radio/storage producers may still be executing outside
     * the console task. Actual attempts are counted by boot-time resume. */
    journal.phase = D1L_FACTORY_RESET_JOURNAL_ACTIVE;
    journal.completed_domains = 0U;
    journal.next_domain = 0U;
    journal.last_failed_domain = UINT32_MAX;
    journal.last_error = ESP_OK;
    journal.erased_count = 0U;
    journal.absent_count = 0U;
    journal.flags = 0U;
    journal_store_outcome_t store_outcome = {0};
    const esp_err_t checkpoint_ret = journal_store_with_outcome(
        &journal, &store_outcome);
    out_status->request_commit_attempted = store_outcome.commit_attempted;
    out_status->request_write_may_have_applied =
        store_outcome.write_may_have_applied;
    if (checkpoint_ret != ESP_OK) {
        if (!store_outcome.write_may_have_applied) {
            set_storage_failure(out_status, checkpoint_ret, "reset_journal");
            out_status->reset_pending = false;
            return checkpoint_ret;
        }

        /* nvs_set_blob() and nvs_commit() errors may be fail-after-apply.
         * Classify the durable state immediately while every producer remains
         * quiesced. Any
         * non-exact result remains ambiguous and requires a controlled
         * restart; a valid ACTIVE readback still requires restart because
         * early boot will execute it. */
        d1l_factory_reset_journal_t readback = {0};
        esp_err_t readback_error = ESP_OK;
        const journal_load_state_t readback_state = journal_load(
            &readback, &readback_error);
        out_status->request_commit_attempted = store_outcome.commit_attempted;
        out_status->request_write_may_have_applied = true;
        out_status->request_outcome_ambiguous = true;
        if (readback_state == JOURNAL_LOAD_ACTIVE) {
            status_from_journal(&readback, out_status);
            out_status->request_commit_attempted =
                store_outcome.commit_attempted;
            out_status->request_write_may_have_applied = true;
            out_status->request_readback_exact =
                memcmp(&readback, &journal, sizeof(readback)) == 0;
            out_status->request_outcome_ambiguous =
                !out_status->request_readback_exact;
        } else if (readback_state == JOURNAL_LOAD_COMPLETE) {
            status_from_journal(&readback, out_status);
            out_status->request_commit_attempted =
                store_outcome.commit_attempted;
            out_status->request_write_may_have_applied = true;
            out_status->request_outcome_ambiguous = true;
        } else if (readback_state == JOURNAL_LOAD_NEWER ||
                   readback_state == JOURNAL_LOAD_CORRUPT ||
                   readback_state == JOURNAL_LOAD_ERROR) {
            (void)quarantine_or_error_status(
                readback_state, readback_error, out_status);
            out_status->request_commit_attempted =
                store_outcome.commit_attempted;
            out_status->request_write_may_have_applied = true;
            out_status->request_outcome_ambiguous = true;
        } else {
            set_storage_failure(out_status, checkpoint_ret, "reset_journal");
            out_status->request_commit_attempted =
                store_outcome.commit_attempted;
            out_status->request_write_may_have_applied = true;
            out_status->request_outcome_ambiguous = true;
        }
        out_status->last_error = checkpoint_ret;
        return checkpoint_ret;
    }
    status_from_journal(&journal, out_status);
    populate_sd_lineage_status(out_status);
    return ESP_OK;
}

esp_err_t d1l_factory_reset_repair_quarantined(
    d1l_factory_reset_status_t *out_status)
{
    if (!out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_status = status_defaults();
    sd_lineage_cache_reset();

    d1l_factory_reset_journal_t quarantined = {0};
    esp_err_t load_error = ESP_OK;
    const journal_load_state_t load_state = journal_load(
        &quarantined, &load_error);
    if (load_state != JOURNAL_LOAD_NEWER &&
        load_state != JOURNAL_LOAD_CORRUPT &&
        load_state != JOURNAL_LOAD_ERROR) {
        if (load_state == JOURNAL_LOAD_ACTIVE ||
            load_state == JOURNAL_LOAD_COMPLETE) {
            status_from_journal(&quarantined, out_status);
        }
        populate_sd_lineage_status(out_status);
        return ESP_ERR_INVALID_STATE;
    }

    d1l_factory_reset_journal_t journal = {
        .magic = D1L_FACTORY_RESET_JOURNAL_MAGIC,
        .schema = D1L_FACTORY_RESET_JOURNAL_SCHEMA,
        .phase = D1L_FACTORY_RESET_JOURNAL_ACTIVE,
        .last_failed_domain = UINT32_MAX,
        .last_error = ESP_OK,
        .flags = D1L_FACTORY_RESET_JOURNAL_FLAG_EXPLICIT_REPAIR,
    };
    const esp_err_t generation_ret = next_sd_lineage_generation(
        true, &journal.sd_lineage_generation);
    if (generation_ret != ESP_OK) {
        set_storage_failure(out_status, generation_ret,
                            "sd_lineage_fence");
        populate_sd_lineage_status(out_status);
        return generation_ret;
    }

    journal_store_outcome_t outcome = {0};
    const esp_err_t store_ret = journal_store_with_outcome(
        &journal, &outcome);
    out_status->request_commit_attempted = outcome.commit_attempted;
    out_status->request_write_may_have_applied =
        outcome.write_may_have_applied;
    if (store_ret != ESP_OK) {
        set_storage_failure(out_status, store_ret, "reset_journal");
        out_status->request_outcome_ambiguous =
            outcome.write_may_have_applied;
        populate_sd_lineage_status(out_status);
        return store_ret;
    }
    d1l_factory_reset_journal_t readback = {0};
    esp_err_t readback_error = ESP_OK;
    const journal_load_state_t readback_state = journal_load(
        &readback, &readback_error);
    if (readback_state != JOURNAL_LOAD_ACTIVE ||
        memcmp(&readback, &journal, sizeof(readback)) != 0) {
        if (readback_state == JOURNAL_LOAD_ACTIVE ||
            readback_state == JOURNAL_LOAD_COMPLETE) {
            status_from_journal(&readback, out_status);
        } else {
            (void)quarantine_or_error_status(
                readback_state, readback_error, out_status);
        }
        out_status->request_write_may_have_applied = true;
        out_status->request_outcome_ambiguous = true;
        populate_sd_lineage_status(out_status);
        return readback_error == ESP_OK ? ESP_ERR_INVALID_STATE :
                                          readback_error;
    }
    status_from_journal(&readback, out_status);
    out_status->request_readback_exact = true;
    populate_sd_lineage_status(out_status);
    return ESP_OK;
}
