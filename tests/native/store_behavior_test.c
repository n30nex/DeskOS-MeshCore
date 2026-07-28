#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mesh/contact_store.h"
#include "mesh/meshcore_advert_admission.h"
#include "mesh/meshcore_lifetime.h"
#include "mesh/meshcore_packet_hash.h"
#include "mesh/node_store.h"
#include "mock_esp_nvs.h"
#include "storage/retained_blob_store.h"

#define NODE_NAMESPACE "d1l_nodes"
#define NODE_KEY "heard"
#define CONTACT_NAMESPACE "d1l_contacts"
#define CONTACT_KEY "contacts"
#define LEGACY_ALIAS_LEN 24U
#define LEGACY_TYPE_LEN 8U
#define NODE_SD_BLOB_MAX (128U * 1024U)

static uint8_t s_node_sd_blob[NODE_SD_BLOB_MAX];
static size_t s_node_sd_blob_len;
static esp_err_t s_node_sd_fail_next_write;

static void native_store_reset(void)
{
    mock_nvs_reset();
    memset(s_node_sd_blob, 0, sizeof(s_node_sd_blob));
    s_node_sd_blob_len = 0U;
    s_node_sd_fail_next_write = ESP_OK;
}

#define mock_nvs_reset native_store_reset

static void mock_node_sd_fail_next_write(esp_err_t error)
{
    s_node_sd_fail_next_write = error;
}

bool d1l_retained_blob_store_backend_state(
    d1l_retained_blob_store_id_t store_id,
    d1l_retained_blob_store_backend_state_t *out_state)
{
    if (store_id != D1L_RETAINED_BLOB_STORE_NODES || !out_state) {
        return false;
    }
    out_state->enabled = true;
    out_state->generation = 1U;
    return true;
}

esp_err_t d1l_retained_blob_store_read_sd_primary(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    if (store_id != D1L_RETAINED_BLOB_STORE_NODES || !key || !dst ||
        !len_inout || strcmp(key, "nodes_v1") != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_node_sd_blob_len == 0U) {
        return ESP_ERR_NOT_FOUND;
    }
    if (*len_inout < s_node_sd_blob_len) {
        *len_inout = s_node_sd_blob_len;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, s_node_sd_blob, s_node_sd_blob_len);
    *len_inout = s_node_sd_blob_len;
    return ESP_OK;
}

esp_err_t d1l_retained_blob_store_write_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len, uint32_t expected_generation)
{
    if (store_id != D1L_RETAINED_BLOB_STORE_NODES || !key || !src ||
        strcmp(key, "nodes_v1") != 0 || expected_generation != 1U ||
        len == 0U || len > sizeof(s_node_sd_blob)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_node_sd_fail_next_write != ESP_OK) {
        const esp_err_t failure = s_node_sd_fail_next_write;
        s_node_sd_fail_next_write = ESP_OK;
        return failure;
    }
    memcpy(s_node_sd_blob, src, len);
    s_node_sd_blob_len = len;
    return ESP_OK;
}

static const char KEY_HEX[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void make_public_key(char key[D1L_NODE_PUBLIC_KEY_HEX_LEN], uint32_t id);
static void fingerprint_from_key(
    char fingerprint[D1L_NODE_FINGERPRINT_LEN],
    const char key[D1L_NODE_PUBLIC_KEY_HEX_LEN]);
static void uppercase_hex(char *dest, size_t dest_size, const char *src);

typedef struct {
    uint32_t seq;
    uint32_t first_heard_ms;
    uint32_t last_heard_ms;
    uint32_t advert_timestamp;
    uint32_t heard_count;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    char name[D1L_HEARD_NODE_NAME_LEN];
    char type[LEGACY_TYPE_LEN];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
} node_entry_v3_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    node_entry_v3_t entries[D1L_NODE_NVS_FALLBACK_CAPACITY];
} node_blob_v3_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_node_entry_t entries[D1L_NODE_NVS_FALLBACK_CAPACITY];
} node_blob_v4_t;

typedef struct {
    uint32_t seq;
    uint32_t created_ms;
    uint32_t updated_ms;
    uint32_t out_path_updated_ms;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    char alias[LEGACY_ALIAS_LEN];
    char heard_name[D1L_HEARD_NODE_NAME_LEN];
    char type[LEGACY_TYPE_LEN];
    int last_rssi_dbm;
    int last_snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool out_path_valid;
    uint8_t out_path_len;
    uint8_t out_path[D1L_CONTACT_OUT_PATH_MAX];
    bool favorite;
    bool muted;
} contact_entry_v3_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    contact_entry_v3_t entries[D1L_CONTACT_STORE_CAPACITY];
} contact_blob_v3_t;

typedef struct {
    uint32_t seq;
    uint32_t created_ms;
    uint32_t updated_ms;
    uint32_t out_path_updated_ms;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    char alias[LEGACY_ALIAS_LEN];
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
} contact_entry_v4_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    contact_entry_v4_t entries[D1L_CONTACT_STORE_CAPACITY];
} contact_blob_v4_t;

typedef struct {
    uint32_t seq;
    uint32_t created_ms;
    uint32_t updated_ms;
    uint32_t out_path_updated_ms;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    char alias[LEGACY_ALIAS_LEN];
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
} contact_entry_v5_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    contact_entry_v5_t entries[D1L_CONTACT_STORE_CAPACITY];
} contact_blob_v5_t;

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
} contact_entry_v6_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    contact_entry_v6_t entries[D1L_CONTACT_STORE_CAPACITY];
} contact_blob_v6_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t count;
    d1l_contact_entry_t entries[D1L_CONTACT_STORE_CAPACITY];
} contact_blob_v7_t;

_Static_assert(sizeof(node_entry_v3_t) == 148U,
               "native node v3 fixture no longer matches production migration layout");
_Static_assert(sizeof(contact_entry_v3_t) == 236U,
               "native contact v3 fixture no longer matches production migration layout");
_Static_assert(sizeof(contact_entry_v4_t) == 236U,
               "native contact v4 fixture no longer matches production migration layout");
_Static_assert(sizeof(contact_entry_v5_t) == 248U,
               "native contact v5 fixture no longer matches migration layout");
_Static_assert(sizeof(contact_entry_v6_t) == 256U,
                "native contact v6 fixture no longer matches migration layout");
_Static_assert(sizeof(d1l_contact_entry_t) == 280U,
                "native contact v7 fixture no longer matches production layout");
_Static_assert(sizeof(KEY_HEX) == D1L_NODE_PUBLIC_KEY_HEX_LEN,
               "test public key must be exactly 64 hex characters");

static void copy_field(char *dest, size_t dest_size, const char *src)
{
    assert(dest != NULL);
    assert(dest_size > 0U);
    memset(dest, 0, dest_size);
    if (!src) {
        return;
    }
    size_t length = strlen(src);
    if (length > dest_size) {
        length = dest_size;
    }
    memcpy(dest, src, length);
}

static void assert_node_stats_equal(d1l_node_store_stats_t left,
                                    d1l_node_store_stats_t right)
{
    assert(left.next_seq == right.next_seq);
    assert(left.total_written == right.total_written);
    assert(left.dropped_oldest == right.dropped_oldest);
    assert(left.count == right.count);
    assert(left.capacity == right.capacity);
}

static void assert_contact_stats_equal(d1l_contact_store_stats_t left,
                                       d1l_contact_store_stats_t right)
{
    assert(left.next_seq == right.next_seq);
    assert(left.total_written == right.total_written);
    assert(left.dropped_oldest == right.dropped_oldest);
    assert(left.count == right.count);
    assert(left.capacity == right.capacity);
}

static void assert_contact_stats_exact(d1l_contact_store_stats_t left,
                                       d1l_contact_store_stats_t right)
{
    assert_contact_stats_equal(left, right);
    assert(left.persistence_revision == right.persistence_revision);
    assert(left.persistence_commit_count == right.persistence_commit_count);
    assert(left.persistence_coalesced_count == right.persistence_coalesced_count);
    assert(left.persistence_fail_count == right.persistence_fail_count);
    assert(left.persistence_last_error == right.persistence_last_error);
    assert(left.persistence_dirty == right.persistence_dirty);
}

static void fill_node_v3(node_entry_v3_t *entry, uint32_t seq,
                         const char *fingerprint, const char *name,
                         const char *legacy_type)
{
    memset(entry, 0, sizeof(*entry));
    entry->seq = seq;
    entry->first_heard_ms = seq * 100U;
    entry->last_heard_ms = seq * 200U;
    entry->advert_timestamp = seq * 10U;
    entry->heard_count = seq;
    copy_field(entry->fingerprint, sizeof(entry->fingerprint), fingerprint);
    copy_field(entry->public_key_hex, sizeof(entry->public_key_hex), KEY_HEX);
    copy_field(entry->name, sizeof(entry->name), name);
    copy_field(entry->type, sizeof(entry->type), legacy_type);
    entry->rssi_dbm = -40 - (int)seq;
    entry->snr_tenths = 100 + (int)seq;
    entry->path_hash_bytes = 1U;
    entry->path_hops = 2U;
}

static void fill_contact_v3(contact_entry_v3_t *entry, uint32_t seq,
                            const char *fingerprint, const char *alias,
                            const char *legacy_type)
{
    memset(entry, 0, sizeof(*entry));
    entry->seq = seq;
    entry->created_ms = seq * 100U;
    entry->updated_ms = seq * 200U;
    entry->out_path_updated_ms = seq * 300U;
    copy_field(entry->fingerprint, sizeof(entry->fingerprint), fingerprint);
    copy_field(entry->public_key_hex, sizeof(entry->public_key_hex), KEY_HEX);
    copy_field(entry->alias, sizeof(entry->alias), alias);
    copy_field(entry->heard_name, sizeof(entry->heard_name), alias);
    copy_field(entry->type, sizeof(entry->type), legacy_type);
    entry->last_rssi_dbm = -50 - (int)seq;
    entry->last_snr_tenths = 80 + (int)seq;
    entry->path_hash_bytes = 2U;
    entry->path_hops = 3U;
    entry->out_path_valid = true;
    entry->out_path_len = 2U;
    entry->out_path[0] = 0xa5U;
    entry->out_path[1] = 0x5aU;
    entry->favorite = true;
    entry->muted = (seq & 1U) == 0U;
}

static void fill_contact_v4(contact_entry_v4_t *entry, uint32_t seq,
                            const char *fingerprint, const char *public_key,
                            const char *alias, const char *type)
{
    memset(entry, 0, sizeof(*entry));
    entry->seq = seq;
    entry->created_ms = seq * 100U;
    entry->updated_ms = seq * 200U;
    entry->out_path_updated_ms = seq * 300U;
    copy_field(entry->fingerprint, sizeof(entry->fingerprint), fingerprint);
    copy_field(entry->public_key_hex, sizeof(entry->public_key_hex), public_key);
    copy_field(entry->alias, sizeof(entry->alias), alias);
    copy_field(entry->heard_name, sizeof(entry->heard_name), alias);
    copy_field(entry->type, sizeof(entry->type), type);
    entry->last_rssi_dbm = -50 - (int)seq;
    entry->last_snr_tenths = 80 + (int)seq;
    entry->path_hash_bytes = 2U;
    entry->path_hops = 3U;
    entry->out_path_valid = true;
    entry->out_path_len = 2U;
    entry->out_path[0] = 0xa5U;
    entry->out_path[1] = 0x5aU;
    entry->favorite = true;
    entry->muted = (seq & 1U) == 0U;
}

static void test_node_v3_to_v4_migration(void)
{
    mock_nvs_reset();
    node_blob_v3_t legacy = {0};
    legacy.schema = 3U;
    legacy.next_seq = 10U;
    legacy.total_written = 21U;
    legacy.dropped_oldest = 2U;
    legacy.count = 3U;
    fill_node_v3(&legacy.entries[0], 1U, "1111111111111111", "Alice", "chat");
    fill_node_v3(&legacy.entries[1], 2U, "2222222222222222", "Hill", "room");
    fill_node_v3(&legacy.entries[2], 3U, "3333333333333333", "Weather", "sensor");
    assert(mock_nvs_seed_blob(NODE_NAMESPACE, NODE_KEY, &legacy, sizeof(legacy)));

    assert(d1l_node_store_init() == ESP_OK);
    d1l_node_store_stats_t stats = d1l_node_store_stats();
    assert(stats.next_seq == 10U);
    assert(stats.total_written == 21U);
    assert(stats.dropped_oldest == 2U);
    assert(stats.count == 3U);

    d1l_node_entry_t node = {0};
    assert(d1l_node_store_find_by_fingerprint("1111111111111111", &node));
    assert(strcmp(node.type, "chat") == 0);
    assert(!node.location_valid);
    assert(node.location_advert_timestamp == 0U);
    assert(node.location_seq == 0U);
    assert(d1l_node_store_find_by_fingerprint("2222222222222222", &node));
    assert(strcmp(node.type, "repeater") == 0);
    assert(!node.location_valid);
    assert(d1l_node_store_find_by_fingerprint("3333333333333333", &node));
    assert(strcmp(node.type, "unknown") == 0);
    assert(!node.location_valid);

    assert(stats.persistence_dirty);
    assert(d1l_node_store_flush() == ESP_OK);
    node_blob_v4_t current = {0};
    assert(mock_nvs_copy_blob(NODE_NAMESPACE, NODE_KEY, &current,
                              sizeof(current)) == 0U);
    assert(d1l_node_store_init() == ESP_OK);
    assert(d1l_node_store_find_by_fingerprint("3333333333333333", &node));
    assert(strcmp(node.type, "unknown") == 0);
}

static void test_contact_v3_to_v7_migration(void)
{
    mock_nvs_reset();
    contact_blob_v3_t legacy = {0};
    legacy.schema = 3U;
    legacy.next_seq = 9U;
    legacy.total_written = 14U;
    legacy.dropped_oldest = 1U;
    legacy.count = 2U;
    fill_contact_v3(&legacy.entries[0], 4U, "4444444444444444", "Legacy Room", "room");
    fill_contact_v3(&legacy.entries[1], 5U, "5555555555555555", "Ambiguous", "sensor");
    assert(mock_nvs_seed_blob(CONTACT_NAMESPACE, CONTACT_KEY, &legacy, sizeof(legacy)));

    assert(d1l_contact_store_init() == ESP_OK);
    d1l_contact_store_stats_t stats = d1l_contact_store_stats();
    assert(stats.next_seq == 9U);
    assert(stats.total_written == 14U);
    assert(stats.dropped_oldest == 1U);
    assert(stats.count == 2U);

    d1l_contact_entry_t contact = {0};
    assert(d1l_contact_store_find_by_fingerprint("4444444444444444", &contact));
    assert(strcmp(contact.type, "repeater") == 0);
    assert(contact.verification_source == D1L_CONTACT_VERIFICATION_NONE);
    assert(contact.verified_at_ms == 0U);
    assert(!d1l_contact_store_is_canonical(&contact));
    assert(contact.out_path_valid);
    assert(contact.out_path_len == 2U);
    assert(contact.out_path[0] == 0xa5U && contact.out_path[1] == 0x5aU);
    assert(contact.favorite);
    assert(contact.out_path_updated_ms == 1200U);
    assert(contact.out_path_state.lifecycle == D1L_MESHCORE_PATH_STATE_VALID);
    assert(contact.out_path_state.source == D1L_MESHCORE_PATH_SOURCE_MIGRATED);
    assert(contact.out_path_state.generation == 1U);

    assert(d1l_contact_store_find_by_fingerprint("5555555555555555", &contact));
    assert(strcmp(contact.type, "unknown") == 0);
    char uri[D1L_CONTACT_EXPORT_URI_LEN] = "must-be-cleared";
    assert(d1l_contact_store_export_uri(&contact, uri, sizeof(uri)) ==
           ESP_ERR_INVALID_STATE);
    assert(uri[0] == '\0');

    contact_blob_v7_t current = {0};
    assert(mock_nvs_copy_blob(CONTACT_NAMESPACE, CONTACT_KEY, &current,
                              sizeof(current)) == sizeof(current));
    assert(current.schema == 7U);
    assert(current.count == 2U);
    assert(d1l_contact_store_init() == ESP_OK);
    assert(d1l_contact_store_find_by_fingerprint("4444444444444444", &contact));
    assert(strcmp(contact.type, "repeater") == 0);
}

static void test_contact_v4_to_v7_migration_is_truthful(void)
{
    mock_nvs_reset();
    contact_blob_v4_t legacy = {0};
    legacy.schema = 4U;
    legacy.next_seq = 12U;
    legacy.total_written = 9U;
    legacy.count = 2U;

    char valid_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char valid_fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    char retained_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char retained_fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(valid_key, 0x51U);
    fingerprint_from_key(valid_fingerprint, valid_key);
    uppercase_hex(retained_key, sizeof(retained_key), valid_key);
    uppercase_hex(retained_fingerprint, sizeof(retained_fingerprint),
                  valid_fingerprint);
    fill_contact_v4(&legacy.entries[0], 7U, retained_fingerprint, retained_key,
                    "Migrated", "chat");
    fill_contact_v4(&legacy.entries[1], 8U, "ffffffffffffffff", "",
                    "Placeholder", "unknown");
    assert(mock_nvs_seed_blob(CONTACT_NAMESPACE, CONTACT_KEY, &legacy,
                              sizeof(legacy)));

    assert(d1l_contact_store_init() == ESP_OK);
    d1l_contact_entry_t contact = {0};
    assert(d1l_contact_store_find_by_fingerprint(valid_fingerprint, &contact));
    assert(strcmp(contact.fingerprint, retained_fingerprint) == 0);
    assert(contact.verification_source ==
           D1L_CONTACT_VERIFICATION_MIGRATED_SIGNED_ADVERT);
    assert(contact.verified_at_ms == 1400U);
    assert(contact.signed_advert_timestamp == 0U);
    assert(contact.last_heard_ms == 0U);
    assert(d1l_contact_store_is_canonical(&contact));
    assert(d1l_contact_store_can_dm(&contact));
    assert(!d1l_contact_store_can_admin(&contact));

    assert(d1l_contact_store_find_by_fingerprint("ffffffffffffffff", &contact));
    assert(contact.verification_source == D1L_CONTACT_VERIFICATION_NONE);
    assert(!d1l_contact_store_is_canonical(&contact));
    assert(!d1l_contact_store_can_dm(&contact));

    contact_blob_v7_t current = {0};
    assert(mock_nvs_copy_blob(CONTACT_NAMESPACE, CONTACT_KEY, &current,
                              sizeof(current)) == sizeof(current));
    assert(current.schema == 7U);
    assert(current.count == 2U);
}

static void test_contact_v5_to_v7_migration_preserves_provenance(void)
{
    mock_nvs_reset();
    contact_blob_v5_t legacy = {0};
    legacy.schema = 5U;
    legacy.next_seq = 18U;
    legacy.total_written = 13U;
    legacy.dropped_oldest = 2U;
    legacy.count = 1U;

    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x59U);
    fingerprint_from_key(fingerprint, key);
    contact_entry_v5_t *old = &legacy.entries[0];
    old->seq = 17U;
    old->created_ms = 1000U;
    old->updated_ms = 2000U;
    old->out_path_updated_ms = 1500U;
    copy_field(old->fingerprint, sizeof(old->fingerprint), fingerprint);
    copy_field(old->public_key_hex, sizeof(old->public_key_hex), key);
    copy_field(old->alias, sizeof(old->alias), "12345678901234567890123");
    copy_field(old->heard_name, sizeof(old->heard_name), "Heard");
    copy_field(old->type, sizeof(old->type), "chat");
    old->last_rssi_dbm = -67;
    old->last_snr_tenths = 42;
    old->path_hash_bytes = 1U;
    old->path_hops = 2U;
    old->out_path_valid = true;
    old->out_path_len = 2U;
    old->out_path[0] = 0x12U;
    old->out_path[1] = 0x34U;
    old->favorite = true;
    old->muted = true;
    old->verification_source = D1L_CONTACT_VERIFICATION_URI_IMPORT;
    old->verified_at_ms = 1900U;
    old->signed_advert_timestamp = 0U;
    old->last_heard_ms = 1700U;
    assert(mock_nvs_seed_blob(CONTACT_NAMESPACE, CONTACT_KEY, &legacy,
                              sizeof(legacy)));

    assert(d1l_contact_store_init() == ESP_OK);
    d1l_contact_entry_t contact = {0};
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &contact));
    assert(strcmp(contact.alias, "12345678901234567890123") == 0);
    assert(strcmp(contact.public_key_hex, key) == 0);
    assert(contact.verification_source == D1L_CONTACT_VERIFICATION_URI_IMPORT);
    assert(contact.verified_at_ms == 1900U);
    assert(contact.last_heard_ms == 1700U);
    assert(contact.favorite && contact.muted);
    assert(contact.out_path_valid && contact.out_path_len == 2U);
    assert(contact.out_path[0] == 0x12U && contact.out_path[1] == 0x34U);
    assert(contact.out_path_state.lifecycle == D1L_MESHCORE_PATH_STATE_VALID);
    assert(contact.out_path_state.source == D1L_MESHCORE_PATH_SOURCE_MIGRATED);
    assert(d1l_contact_store_can_dm(&contact));

    contact_blob_v7_t current = {0};
    assert(mock_nvs_copy_blob(CONTACT_NAMESPACE, CONTACT_KEY, &current,
                              sizeof(current)) == sizeof(current));
    assert(current.schema == 7U);
    assert(current.next_seq == 18U);
    assert(current.total_written == 13U);
    assert(current.dropped_oldest == 2U);
}

static void test_contact_v6_to_v7_migration_creates_preboot_path_record(void)
{
    mock_nvs_reset();
    contact_blob_v6_t legacy = {0};
    legacy.schema = 6U;
    legacy.next_seq = 3U;
    legacy.total_written = 2U;
    legacy.count = 1U;
    contact_entry_v6_t *old = &legacy.entries[0];
    old->seq = 2U;
    old->created_ms = 100U;
    old->updated_ms = 200U;
    old->out_path_updated_ms = 150U;
    copy_field(old->fingerprint, sizeof(old->fingerprint),
               "6161616161616161");
    copy_field(old->alias, sizeof(old->alias), "v6 route");
    copy_field(old->type, sizeof(old->type), "chat");
    old->out_path_valid = true;
    old->out_path_len = 1U;
    old->out_path[0] = 0x61U;
    assert(mock_nvs_seed_blob(CONTACT_NAMESPACE, CONTACT_KEY, &legacy,
                              sizeof(legacy)));

    assert(d1l_contact_store_init() == ESP_OK);
    d1l_contact_entry_t contact = {0};
    assert(d1l_contact_store_find_by_fingerprint(
        "6161616161616161", &contact));
    assert(contact.out_path_valid);
    assert(contact.out_path_state.lifecycle == D1L_MESHCORE_PATH_STATE_VALID);
    assert(contact.out_path_state.source == D1L_MESHCORE_PATH_SOURCE_MIGRATED);
    assert(contact.out_path_state.learned_at_ms == 150U);
    assert(contact.out_path_state.generation == 1U);

    contact_blob_v7_t current = {0};
    assert(mock_nvs_copy_blob(CONTACT_NAMESPACE, CONTACT_KEY, &current,
                              sizeof(current)) == sizeof(current));
    assert(current.schema == 7U);
}

static void test_canonical_path_lifecycle_is_atomic_and_retained(void)
{
    static const char fingerprint[] = "7070707070707070";
    const uint8_t path[] = {0x10U, 0x11U, 0x20U, 0x21U};
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);
    assert(d1l_contact_store_upsert_from_node(
        fingerprint, "route peer", NULL) == ESP_OK);

    mock_timer_set_us(1000LL * 1000LL);
    d1l_contact_entry_t contact = {0};
    assert(d1l_contact_store_update_path_from_source(
        fingerprint, path, 0x42U, D1L_MESHCORE_PATH_SOURCE_ACK_PATH,
        &contact) == ESP_OK);
    const uint32_t first_generation = contact.out_path_state.generation;
    assert(first_generation != 0U);
    assert(contact.out_path_valid && contact.out_path_len == 0x42U);
    assert(memcmp(contact.out_path, path, sizeof(path)) == 0);
    assert(contact.out_path_state.source == D1L_MESHCORE_PATH_SOURCE_ACK_PATH);
    assert(contact.out_path_state.lifecycle == D1L_MESHCORE_PATH_STATE_VALID);
    assert(contact.out_path_state.learned_at_ms == 1000U);
    assert(contact.out_path_state.expires_at_ms ==
           1000U + D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS);
    d1l_contact_store_stats_t path_stats = d1l_contact_store_stats();
    assert(path_stats.persistence_dirty);
    contact_blob_v7_t persisted_before_flush = {0};
    assert(mock_nvs_copy_blob(
        CONTACT_NAMESPACE, CONTACT_KEY, &persisted_before_flush,
        sizeof(persisted_before_flush)) == sizeof(persisted_before_flush));
    assert(!persisted_before_flush.entries[0].out_path_valid);
    assert(persisted_before_flush.entries[0].out_path_state.lifecycle ==
           D1L_MESHCORE_PATH_STATE_NONE);
    assert(d1l_contact_store_flush() == ESP_OK);
    assert(!d1l_contact_store_stats().persistence_dirty);

    d1l_meshcore_path_result_t result = D1L_MESHCORE_PATH_RESULT_STALE;
    assert(d1l_contact_store_note_path_result(
        fingerprint, first_generation, true, 2000U, &contact,
        &result) == ESP_OK);
    assert(result == D1L_MESHCORE_PATH_RESULT_UPDATED);
    assert(contact.out_path_state.last_success_ms == 2000U);
    assert(contact.out_path_state.consecutive_failures == 0U);

    assert(d1l_contact_store_note_path_result(
        fingerprint, first_generation, false, 3000U, &contact,
        &result) == ESP_OK);
    assert(result == D1L_MESHCORE_PATH_RESULT_UPDATED);
    assert(contact.out_path_valid);
    assert(contact.out_path_state.consecutive_failures == 1U);

    assert(d1l_contact_store_note_path_result(
        fingerprint, first_generation, false, 4000U, &contact,
        &result) == ESP_OK);
    assert(result == D1L_MESHCORE_PATH_RESULT_FLOOD_FALLBACK);
    assert(!contact.out_path_valid && contact.out_path_len == 0U);
    assert(contact.out_path_state.lifecycle == D1L_MESHCORE_PATH_STATE_FAILED);
    assert(contact.out_path_state.generation != first_generation);
    for (size_t i = 0U; i < sizeof(contact.out_path); ++i) {
        assert(contact.out_path[i] == 0U);
    }
    assert(d1l_contact_store_stats().persistence_dirty);
    assert(d1l_contact_store_flush() == ESP_OK);

    assert(d1l_contact_store_init() == ESP_OK);
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &contact));
    assert(contact.out_path_state.lifecycle == D1L_MESHCORE_PATH_STATE_FAILED);
    assert(!contact.out_path_valid);

    mock_timer_set_us(5000LL * 1000LL);
    const uint8_t replacement[] = {0x77U};
    assert(d1l_contact_store_update_path_from_source(
        fingerprint, replacement, 0x01U,
        D1L_MESHCORE_PATH_SOURCE_PATH_RESPONSE, &contact) == ESP_OK);
    const uint32_t replacement_generation = contact.out_path_state.generation;
    assert(replacement_generation != first_generation);
    assert(contact.out_path_state.source ==
           D1L_MESHCORE_PATH_SOURCE_PATH_RESPONSE);

    const d1l_contact_entry_t replacement_snapshot = contact;
    result = D1L_MESHCORE_PATH_RESULT_UPDATED;
    assert(d1l_contact_store_note_path_result(
        fingerprint, first_generation, false, 6000U, &contact,
        &result) == ESP_ERR_INVALID_STATE);
    assert(result == D1L_MESHCORE_PATH_RESULT_STALE);
    assert(memcmp(&contact, &replacement_snapshot, sizeof(contact)) == 0);

    bool expired = true;
    assert(d1l_contact_store_prepare_path_route(
        fingerprint,
        5000U + D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS,
        &contact, &expired) == ESP_OK);
    assert(!expired && contact.out_path_valid);
    assert(d1l_contact_store_prepare_path_route(
        fingerprint,
        5001U + D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS,
        &contact, &expired) == ESP_OK);
    assert(expired);
    assert(!contact.out_path_valid);
    assert(contact.out_path_state.lifecycle == D1L_MESHCORE_PATH_STATE_EXPIRED);
    assert(d1l_contact_store_flush() == ESP_OK);

    mock_timer_set_us(7000LL * 1000LL);
    assert(d1l_contact_store_update_path_from_source(
        fingerprint, replacement, 0x01U,
        D1L_MESHCORE_PATH_SOURCE_OBSERVED, &contact) == ESP_OK);
    mock_timer_set_us(7001LL * 1000LL);
    assert(d1l_contact_store_note_path_result(
        fingerprint, contact.out_path_state.generation, false, 7001U,
        &contact, &result) == ESP_OK);
    assert(result == D1L_MESHCORE_PATH_RESULT_UPDATED);
    const d1l_contact_entry_t deferred_failure = contact;
    path_stats = d1l_contact_store_stats();
    assert(path_stats.persistence_dirty);
    assert(path_stats.persistence_fail_count == 0U);
    assert(d1l_contact_store_flush_if_due() == ESP_OK);
    assert(d1l_contact_store_stats().persistence_dirty);
    mock_nvs_fail_next_set(ESP_FAIL);
    assert(d1l_contact_store_flush() == ESP_FAIL);
    path_stats = d1l_contact_store_stats();
    assert(path_stats.persistence_dirty);
    assert(path_stats.persistence_fail_count == 1U);
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &contact));
    assert(memcmp(&contact, &deferred_failure, sizeof(contact)) == 0);
    assert(d1l_contact_store_flush() == ESP_OK);
    assert(!d1l_contact_store_stats().persistence_dirty);
    assert(d1l_contact_store_init() == ESP_OK);
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &contact));
    assert(memcmp(&contact, &deferred_failure, sizeof(contact)) == 0);
}

static const char *s_path_flush_race_fingerprint;
static uint32_t s_path_flush_race_generation;
static uint32_t s_path_flush_race_now_ms;
static d1l_contact_entry_t s_path_flush_race_expected;

static void mutate_path_while_flush_is_in_flight(void)
{
    d1l_meshcore_path_result_t result = D1L_MESHCORE_PATH_RESULT_STALE;
    assert(d1l_contact_store_note_path_result(
        s_path_flush_race_fingerprint, s_path_flush_race_generation, true,
        s_path_flush_race_now_ms, &s_path_flush_race_expected,
        &result) == ESP_OK);
    assert(result == D1L_MESHCORE_PATH_RESULT_UPDATED);
}

static void seed_deferred_path_for_flush_race(
    const char *fingerprint, uint32_t *out_generation)
{
    static const uint8_t path[] = {0x42U, 0x24U};
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);
    assert(d1l_contact_store_upsert_from_node(
        fingerprint, "flush race peer", NULL) == ESP_OK);
    mock_timer_set_us(1000LL * 1000LL);
    d1l_contact_entry_t contact = {0};
    assert(d1l_contact_store_update_path_from_source(
        fingerprint, path, 0x02U, D1L_MESHCORE_PATH_SOURCE_ACK_PATH,
        &contact) == ESP_OK);
    assert(contact.out_path_state.generation != 0U);
    assert(d1l_contact_store_stats().persistence_dirty);
    *out_generation = contact.out_path_state.generation;
}

static void test_forced_path_flush_rejects_prewrite_revision_change(void)
{
    static const char fingerprint[] = "7171717171717171";
    uint32_t generation = 0U;
    seed_deferred_path_for_flush_race(fingerprint, &generation);
    const d1l_contact_store_stats_t before = d1l_contact_store_stats();

    s_path_flush_race_fingerprint = fingerprint;
    s_path_flush_race_generation = generation;
    s_path_flush_race_now_ms = 2000U;
    memset(&s_path_flush_race_expected, 0, sizeof(s_path_flush_race_expected));
    /* deferred lock, store lock, then the persistence-I/O lock immediately
     * before the revision compare */
    mock_semaphore_run_after_takes(3U, mutate_path_while_flush_is_in_flight);
    assert(d1l_contact_store_flush() == ESP_ERR_INVALID_STATE);

    d1l_contact_store_stats_t stats = d1l_contact_store_stats();
    assert(stats.persistence_dirty);
    assert(stats.persistence_commit_count == before.persistence_commit_count);
    assert(stats.persistence_coalesced_count ==
           before.persistence_coalesced_count + 1U);
    assert(stats.persistence_fail_count == before.persistence_fail_count + 1U);
    assert(stats.persistence_last_error == ESP_ERR_INVALID_STATE);
    d1l_contact_entry_t in_ram = {0};
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &in_ram));
    assert(memcmp(&in_ram, &s_path_flush_race_expected, sizeof(in_ram)) == 0);

    contact_blob_v7_t stale = {0};
    assert(mock_nvs_copy_blob(
        CONTACT_NAMESPACE, CONTACT_KEY, &stale, sizeof(stale)) == sizeof(stale));
    assert(stale.entries[0].out_path_state.lifecycle ==
           D1L_MESHCORE_PATH_STATE_NONE);

    assert(d1l_contact_store_flush() == ESP_OK);
    assert(!d1l_contact_store_stats().persistence_dirty);
    assert(d1l_contact_store_init() == ESP_OK);
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &in_ram));
    assert(memcmp(&in_ram, &s_path_flush_race_expected, sizeof(in_ram)) == 0);
}

static void test_forced_path_flush_rejects_during_write_revision_change(void)
{
    static const char fingerprint[] = "7272727272727272";
    uint32_t generation = 0U;
    seed_deferred_path_for_flush_race(fingerprint, &generation);
    const d1l_contact_store_stats_t before = d1l_contact_store_stats();

    s_path_flush_race_fingerprint = fingerprint;
    s_path_flush_race_generation = generation;
    s_path_flush_race_now_ms = 3000U;
    memset(&s_path_flush_race_expected, 0, sizeof(s_path_flush_race_expected));
    mock_nvs_run_during_next_set(mutate_path_while_flush_is_in_flight);
    assert(d1l_contact_store_flush() == ESP_ERR_INVALID_STATE);

    d1l_contact_store_stats_t stats = d1l_contact_store_stats();
    assert(stats.persistence_dirty);
    assert(stats.persistence_commit_count == before.persistence_commit_count + 1U);
    assert(stats.persistence_coalesced_count ==
           before.persistence_coalesced_count + 1U);
    assert(stats.persistence_fail_count == before.persistence_fail_count + 1U);
    assert(stats.persistence_last_error == ESP_ERR_INVALID_STATE);

    contact_blob_v7_t stale = {0};
    assert(mock_nvs_copy_blob(
        CONTACT_NAMESPACE, CONTACT_KEY, &stale, sizeof(stale)) == sizeof(stale));
    assert(stale.entries[0].out_path_state.lifecycle ==
           D1L_MESHCORE_PATH_STATE_VALID);
    assert(stale.entries[0].out_path_state.last_success_ms == 0U);
    assert(s_path_flush_race_expected.out_path_state.last_success_ms == 3000U);

    assert(d1l_contact_store_flush() == ESP_OK);
    assert(!d1l_contact_store_stats().persistence_dirty);
    d1l_contact_entry_t reloaded = {0};
    assert(d1l_contact_store_init() == ESP_OK);
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &reloaded));
    assert(memcmp(&reloaded, &s_path_flush_race_expected, sizeof(reloaded)) == 0);
}

static void test_contact_persistence_revision_saturates_fail_closed(void)
{
    mock_nvs_reset();
    mock_timer_set_us(1000LL * 1000LL);
    assert(d1l_contact_store_init() == ESP_OK);
    static const char fingerprint[] = "7373737373737373";
    assert(d1l_contact_store_upsert_from_node(
               fingerprint, "before-boundary", NULL) == ESP_OK);
    assert(d1l_contact_store_test_set_persistence_revision(
               UINT32_MAX - 1U) == ESP_OK);

    d1l_contact_entry_t at_max = {0};
    assert(d1l_contact_store_rename(
               fingerprint, "last-valid-mutation", &at_max) == ESP_OK);
    const d1l_contact_store_stats_t max_stats = d1l_contact_store_stats();
    assert(max_stats.persistence_revision == UINT32_MAX);
    assert(strcmp(at_max.alias, "last-valid-mutation") == 0);
    contact_blob_v7_t durable_at_max = {0};
    assert(mock_nvs_copy_blob(
               CONTACT_NAMESPACE, CONTACT_KEY, &durable_at_max,
               sizeof(durable_at_max)) == sizeof(durable_at_max));

    d1l_contact_entry_t rejected = {0};
    assert(d1l_contact_store_rename(
               fingerprint, "must-not-wrap", &rejected) ==
           ESP_ERR_INVALID_STATE);
    d1l_contact_entry_t unchanged = {0};
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &unchanged));
    assert(memcmp(&unchanged, &at_max, sizeof(unchanged)) == 0);
    const d1l_contact_store_stats_t after = d1l_contact_store_stats();
    assert(after.persistence_revision == UINT32_MAX);
    assert(after.total_written == max_stats.total_written);
    assert(after.persistence_commit_count == max_stats.persistence_commit_count);
    assert(!after.persistence_dirty);
    contact_blob_v7_t durable_after_reject = {0};
    assert(mock_nvs_copy_blob(
               CONTACT_NAMESPACE, CONTACT_KEY, &durable_after_reject,
               sizeof(durable_after_reject)) == sizeof(durable_after_reject));
    assert(memcmp(&durable_after_reject, &durable_at_max,
                  sizeof(durable_at_max)) == 0);

    assert(d1l_contact_store_clear() == ESP_ERR_INVALID_STATE);
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &unchanged));
    assert(memcmp(&unchanged, &at_max, sizeof(unchanged)) == 0);
}

static void test_contact_next_sequence_exhaustion_preserves_ram_and_nvs(void)
{
    mock_nvs_reset();
    mock_timer_set_us(1000LL * 1000LL);
    assert(d1l_contact_store_init() == ESP_OK);
    static const char fingerprint[] = "7474747474747474";
    assert(d1l_contact_store_upsert_from_node(
               fingerprint, "sequence-boundary", NULL) == ESP_OK);

    contact_blob_v7_t exhausted = {0};
    assert(mock_nvs_copy_blob(
               CONTACT_NAMESPACE, CONTACT_KEY, &exhausted,
               sizeof(exhausted)) == sizeof(exhausted));
    exhausted.next_seq = UINT32_MAX;
    assert(mock_nvs_seed_blob(
        CONTACT_NAMESPACE, CONTACT_KEY, &exhausted, sizeof(exhausted)));
    assert(d1l_contact_store_init() == ESP_OK);

    d1l_contact_entry_t before = {0};
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &before));
    const d1l_contact_store_stats_t stats_before = d1l_contact_store_stats();
    assert(stats_before.next_seq == UINT32_MAX);
    contact_blob_v7_t nvs_before = {0};
    assert(mock_nvs_copy_blob(
               CONTACT_NAMESPACE, CONTACT_KEY, &nvs_before,
               sizeof(nvs_before)) == sizeof(nvs_before));

    d1l_contact_entry_t rejected = {0};
    assert(d1l_contact_store_rename(
               fingerprint, "must-not-wrap-sequence", &rejected) ==
           ESP_ERR_INVALID_STATE);
    const uint8_t path[] = {0x42U};
    assert(d1l_contact_store_update_path_from_source(
               fingerprint, path, 0x01U,
               D1L_MESHCORE_PATH_SOURCE_ACK_PATH, &rejected) ==
           ESP_ERR_INVALID_STATE);

    d1l_contact_entry_t after = {0};
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &after));
    assert(memcmp(&after, &before, sizeof(after)) == 0);
    const d1l_contact_store_stats_t stats_after = d1l_contact_store_stats();
    assert_contact_stats_equal(stats_after, stats_before);
    assert(stats_after.persistence_revision == stats_before.persistence_revision);
    assert(stats_after.persistence_commit_count ==
           stats_before.persistence_commit_count);
    assert(stats_after.persistence_fail_count == stats_before.persistence_fail_count);
    assert(!stats_after.persistence_dirty);
    contact_blob_v7_t nvs_after = {0};
    assert(mock_nvs_copy_blob(
               CONTACT_NAMESPACE, CONTACT_KEY, &nvs_after,
               sizeof(nvs_after)) == sizeof(nvs_after));
    assert(memcmp(&nvs_after, &nvs_before, sizeof(nvs_after)) == 0);
}

static void assert_contact_blob_preserved(const void *expected, size_t length)
{
    uint8_t actual[sizeof(contact_blob_v7_t) + 32U] = {0};
    assert(expected != NULL);
    assert(length <= sizeof(actual));
    assert(mock_nvs_copy_blob(CONTACT_NAMESPACE, CONTACT_KEY, actual,
                              sizeof(actual)) == length);
    assert(memcmp(actual, expected, length) == 0);
}

static void test_current_path_record_corruption_is_preserved_fail_closed(void)
{
    mock_nvs_reset();
    contact_blob_v7_t corrupt = {0};
    corrupt.schema = 7U;
    corrupt.next_seq = 2U;
    corrupt.total_written = 1U;
    corrupt.count = 1U;
    corrupt.entries[0].seq = 1U;
    copy_field(corrupt.entries[0].fingerprint,
               sizeof(corrupt.entries[0].fingerprint),
               "7171717171717171");
    corrupt.entries[0].out_path[0] = 0xa5U;
    assert(mock_nvs_seed_blob(CONTACT_NAMESPACE, CONTACT_KEY, &corrupt,
                               sizeof(corrupt)));

    assert(d1l_contact_store_init() == ESP_ERR_INVALID_STATE);
    assert(d1l_contact_store_stats().count == 0U);
    assert(!d1l_contact_store_find_by_fingerprint(
        "7171717171717171", NULL));
    assert_contact_blob_preserved(&corrupt, sizeof(corrupt));
}

static void test_same_size_future_contact_schema_is_preserved_fail_closed(void)
{
    mock_nvs_reset();
    contact_blob_v7_t future = {0};
    future.schema = 8U;
    future.next_seq = 1U;
    assert(mock_nvs_seed_blob(CONTACT_NAMESPACE, CONTACT_KEY, &future,
                              sizeof(future)));

    assert(d1l_contact_store_init() == ESP_ERR_NOT_SUPPORTED);
    assert(d1l_contact_store_stats().count == 0U);
    assert_contact_blob_preserved(&future, sizeof(future));
}

static void test_oversized_future_contact_schema_is_preserved_fail_closed(void)
{
    mock_nvs_reset();
    uint8_t future[sizeof(contact_blob_v7_t) + 16U];
    memset(future, 0x5a, sizeof(future));
    const uint32_t schema = 8U;
    memcpy(future, &schema, sizeof(schema));
    assert(mock_nvs_seed_blob(CONTACT_NAMESPACE, CONTACT_KEY, future,
                              sizeof(future)));

    assert(d1l_contact_store_init() == ESP_ERR_NVS_INVALID_LENGTH);
    assert(d1l_contact_store_stats().count == 0U);
    assert_contact_blob_preserved(future, sizeof(future));
}

static esp_err_t upsert_node(uint32_t advert_timestamp, const char *name,
                             bool location_valid, int32_t lat_e6, int32_t lon_e6,
                             bool *out_stale)
{
    return d1l_node_store_upsert_advert(
        "abcdef0123456789", KEY_HEX, name, 'C', -72, 45, 1U, 2U,
        advert_timestamp, location_valid, lat_e6, lon_e6, out_stale);
}

static void test_node_reachability_is_boot_local_and_wrap_safe(void)
{
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);
    assert(d1l_node_store_init() == ESP_OK);

    /* Uptime zero is a valid fresh observation when validity is explicit. */
    mock_timer_set_us(0);
    bool stale = true;
    assert(upsert_node(1U, "Fresh Zero", false, 0, 0, &stale) == ESP_OK);
    assert(!stale);
    d1l_node_query_t query = {
        .filter = D1L_NODE_FILTER_ALL,
        .sort = D1L_NODE_SORT_LAST_HEARD,
    };
    d1l_node_view_t view = {0};
    assert(d1l_node_store_query(&query, &view, 1U) == 1U);
    assert(view.reachable);
    assert(view.node.last_heard_ms == 0U);

    /* Reload models a new boot: history remains visible, but prior uptime is
     * not allowed to make the node live in the new monotonic epoch. */
    assert(d1l_node_store_flush() == ESP_OK);
    assert(d1l_node_store_init() == ESP_OK);
    memset(&view, 0, sizeof(view));
    assert(d1l_node_store_query(&query, &view, 1U) == 1U);
    assert(!view.reachable);
    assert(view.node.last_heard_ms == 0U);
    query.reachable_only = true;
    assert(d1l_node_store_query(&query, &view, 1U) == 0U);

    /* A current-boot observation remains reachable across uint32 wrap, then
     * expires at max_age + 1 without consulting persisted boot history. */
    const uint32_t wrapped_heard_ms = UINT32_MAX - 10U;
    mock_timer_set_us((int64_t)wrapped_heard_ms * 1000LL);
    stale = true;
    assert(upsert_node(2U, "Fresh Wrap", false, 0, 0, &stale) == ESP_OK);
    assert(!stale);
    mock_timer_set_us(9LL * 1000LL);
    query.reachable_only = false;
    memset(&view, 0, sizeof(view));
    assert(d1l_node_store_query(&query, &view, 1U) == 1U);
    assert(view.reachable);
    assert(view.node.last_heard_ms == wrapped_heard_ms);

    const uint32_t expired_ms = wrapped_heard_ms +
        D1L_MESHCORE_CONTACT_REACHABLE_MAX_AGE_MS + 1U;
    mock_timer_set_us((int64_t)expired_ms * 1000LL);
    memset(&view, 0, sizeof(view));
    assert(d1l_node_store_query(&query, &view, 1U) == 1U);
    assert(!view.reachable);
    assert(view.node.last_heard_ms == wrapped_heard_ms);
}

static void test_stale_advert_and_location_preservation(void)
{
    mock_nvs_reset();
    assert(d1l_node_store_init() == ESP_OK);
    mock_timer_set_us(1000000);
    bool stale = true;
    assert(upsert_node(100U, "Cambridge", true, 43427977, -80316478,
                       &stale) == ESP_OK);
    assert(!stale);

    d1l_node_entry_t original = {0};
    assert(d1l_node_store_find_by_fingerprint("abcdef0123456789", &original));
    const d1l_node_store_stats_t original_stats = d1l_node_store_stats();
    const uint32_t original_generation = d1l_node_store_marker_generation();
    assert(original.location_valid);
    assert(original.location_seq == original.seq);

    mock_timer_set_us(2000000);
    stale = false;
    assert(upsert_node(100U, "Replay", true, 43000000, -80000000,
                       &stale) == ESP_OK);
    assert(stale);
    d1l_node_entry_t after = {0};
    assert(d1l_node_store_find_by_fingerprint("abcdef0123456789", &after));
    assert(memcmp(&after, &original, sizeof(after)) == 0);
    assert_node_stats_equal(d1l_node_store_stats(), original_stats);
    assert(d1l_node_store_marker_generation() == original_generation);

    stale = false;
    assert(upsert_node(99U, "Older", false, 0, 0, &stale) == ESP_OK);
    assert(stale);
    assert(d1l_node_store_find_by_fingerprint("abcdef0123456789", &after));
    assert(memcmp(&after, &original, sizeof(after)) == 0);

    stale = true;
    assert(upsert_node(101U, "Cambridge", false, 0, 0, &stale) == ESP_OK);
    assert(!stale);
    assert(d1l_node_store_find_by_fingerprint("abcdef0123456789", &after));
    assert(after.advert_timestamp == 101U);
    assert(after.seq > original.seq);
    assert(after.location_valid);
    assert(after.lat_e6 == original.lat_e6);
    assert(after.lon_e6 == original.lon_e6);
    assert(after.location_advert_timestamp == original.location_advert_timestamp);
    assert(after.location_seq == original.location_seq);
    assert(d1l_node_store_marker_generation() == original_generation);

    stale = true;
    assert(upsert_node(102U, "Cambridge", true, 43675000, -79440000,
                       &stale) == ESP_OK);
    assert(!stale);
    assert(d1l_node_store_find_by_fingerprint("abcdef0123456789", &after));
    assert(after.lat_e6 == 43675000);
    assert(after.lon_e6 == -79440000);
    assert(after.location_advert_timestamp == 102U);
    assert(after.location_seq == after.seq);
    assert(d1l_node_store_marker_generation() == original_generation + 1U);

    assert(upsert_node(103U, "Cambridge", false, 0, 0, NULL) ==
           ESP_ERR_INVALID_ARG);
}

static void test_node_sd_failure_keeps_live_data_and_retry_succeeds(void)
{
    mock_nvs_reset();
    assert(d1l_node_store_init() == ESP_OK);

    bool stale = true;
    assert(upsert_node(200U, "Retry Node", true, 43427977, -80316478,
                       &stale) == ESP_OK);
    assert(!stale);
    d1l_node_entry_t baseline = {0};
    assert(d1l_node_store_find_by_fingerprint("abcdef0123456789", &baseline));
    mock_node_sd_fail_next_write(ESP_FAIL);
    assert(d1l_node_store_flush() == ESP_FAIL);
    d1l_node_entry_t after = {0};
    assert(d1l_node_store_find_by_fingerprint("abcdef0123456789", &after));
    assert(memcmp(&after, &baseline, sizeof(after)) == 0);
    assert(d1l_node_store_stats().persistence_dirty);

    mock_timer_set_us(
        (int64_t)(D1L_NODE_STORE_PERSIST_MIN_INTERVAL_MS + 1U) * 1000LL);
    assert(d1l_node_store_flush() == ESP_OK);
    assert(d1l_node_store_init() == ESP_OK);
    assert(d1l_node_store_find_by_fingerprint("abcdef0123456789", &after));
    assert(memcmp(&after, &baseline, sizeof(after)) == 0);

    stale = true;
    assert(upsert_node(201U, "Changed", true, 44000000, -81000000,
                       &stale) == ESP_OK);
    assert(!stale);
    assert(d1l_node_store_find_by_fingerprint("abcdef0123456789", &after));
    assert(after.advert_timestamp == 201U);
    assert(strcmp(after.name, "Changed") == 0);
}

static void test_located_nodes_are_retained_until_explicit_clear(void)
{
    mock_nvs_reset();
    assert(d1l_node_store_init() == ESP_OK);

    char first_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char first_fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    for (uint32_t id = 1U; id <= D1L_NODE_STORE_CAPACITY; ++id) {
        char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
        char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
        make_public_key(key, id);
        fingerprint_from_key(fingerprint, key);
        if (id == 1U) {
            memcpy(first_key, key, sizeof(first_key));
            memcpy(first_fingerprint, fingerprint, sizeof(first_fingerprint));
        }
        bool stale = true;
        assert(d1l_node_store_upsert_advert(
                   fingerprint, key, "Mapped", 'C', -72, 45, 1U, 2U, id,
                   true, 43000000 + (int32_t)id,
                   -80000000 - (int32_t)id, &stale) == ESP_OK);
        assert(!stale);
    }

    const d1l_node_store_stats_t full = d1l_node_store_stats();
    assert(full.count == D1L_NODE_STORE_CAPACITY);
    char overflow_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char overflow_fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(overflow_key, D1L_NODE_STORE_CAPACITY + 1U);
    fingerprint_from_key(overflow_fingerprint, overflow_key);
    bool stale = true;
    assert(d1l_node_store_upsert_advert(
               overflow_fingerprint, overflow_key, "Overflow", 'C', -70, 40,
               1U, 2U, D1L_NODE_STORE_CAPACITY + 1U, true, 44000000,
               -81000000, &stale) == ESP_ERR_NO_MEM);
    assert(!stale);
    assert(!d1l_node_store_find_by_fingerprint(overflow_fingerprint, NULL));
    assert_node_stats_equal(d1l_node_store_stats(), full);

    assert(d1l_node_store_upsert_advert(
               first_fingerprint, first_key, "Mapped Updated", 'C', -60, 60,
               1U, 2U, D1L_NODE_STORE_CAPACITY + 2U, false, 0, 0,
               &stale) == ESP_OK);
    assert(!stale);
    d1l_node_entry_t updated = {0};
    assert(d1l_node_store_find_by_fingerprint(first_fingerprint, &updated));
    assert(updated.location_valid);
    assert(strcmp(updated.name, "Mapped Updated") == 0);
}

static d1l_meshcore_advert_admission_receipt_t admit_verified_advert(
    const char *fingerprint, const char *key, const char *name,
    uint32_t timestamp, bool location_valid, int32_t lat_e6, int32_t lon_e6)
{
    d1l_meshcore_advert_admission_receipt_t receipt = {0};
    assert(d1l_meshcore_advert_admit_verified(
               fingerprint, key, name, 'C', -55, 80, 1U, 2U, timestamp,
               location_valid, lat_e6, lon_e6, &receipt) == ESP_OK);
    return receipt;
}

static void initialize_admission_stores(void)
{
    mock_nvs_reset();
    assert(d1l_node_store_init() == ESP_OK);
    assert(d1l_contact_store_init() == ESP_OK);
}

/* d1l_first_zero_timestamp_accepted */
static void test_d1l_first_zero_timestamp_accepted(void)
{
    initialize_admission_stores();
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x31U);
    fingerprint_from_key(fingerprint, key);

    const d1l_meshcore_advert_admission_receipt_t receipt =
        admit_verified_advert(fingerprint, key, "Zero First", 0U, false, 0, 0);
    assert(receipt.outcome == D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED);
    assert(receipt.node_store_error == ESP_OK);
    assert(receipt.contact_store_error == ESP_OK);
    assert(receipt.contact_result == D1L_CONTACT_VERIFIED_ADVERT_CREATED);

    d1l_node_entry_t node = {0};
    d1l_contact_entry_t contact = {0};
    assert(d1l_node_store_find_by_fingerprint(fingerprint, &node));
    assert(d1l_contact_store_find_by_public_key(key, &contact));
    assert(node.advert_timestamp == 0U);
    assert(contact.signed_advert_timestamp == 0U);
}

static void snapshot_admitted_identity(
    const char *fingerprint, const char *key, d1l_node_entry_t *node,
    d1l_contact_entry_t *contact, d1l_node_store_stats_t *node_stats,
    d1l_contact_store_stats_t *contact_stats, uint32_t *marker_generation)
{
    assert(d1l_node_store_find_by_fingerprint(fingerprint, node));
    assert(d1l_contact_store_find_by_public_key(key, contact));
    *node_stats = d1l_node_store_stats();
    *contact_stats = d1l_contact_store_stats();
    *marker_generation = d1l_node_store_marker_generation();
}

static void assert_admitted_identity_unchanged(
    const char *fingerprint, const char *key,
    const d1l_node_entry_t *expected_node,
    const d1l_contact_entry_t *expected_contact,
    d1l_node_store_stats_t expected_node_stats,
    d1l_contact_store_stats_t expected_contact_stats,
    uint32_t expected_marker_generation)
{
    d1l_node_entry_t node = {0};
    d1l_contact_entry_t contact = {0};
    assert(d1l_node_store_find_by_fingerprint(fingerprint, &node));
    assert(d1l_contact_store_find_by_public_key(key, &contact));
    assert(memcmp(&node, expected_node, sizeof(node)) == 0);
    assert(memcmp(&contact, expected_contact, sizeof(contact)) == 0);
    assert_node_stats_equal(d1l_node_store_stats(), expected_node_stats);
    assert_contact_stats_exact(d1l_contact_store_stats(), expected_contact_stats);
    assert(d1l_node_store_marker_generation() == expected_marker_generation);
}

/* d1l_equal_timestamp_rejected_non_mutating */
static void test_d1l_equal_timestamp_rejected_non_mutating(void)
{
    initialize_admission_stores();
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x32U);
    fingerprint_from_key(fingerprint, key);
    assert(admit_verified_advert(fingerprint, key, "Baseline", 100U, true,
                                 43427977, -80316478)
               .outcome == D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED);

    d1l_node_entry_t node = {0};
    d1l_contact_entry_t contact = {0};
    d1l_node_store_stats_t node_stats = {0};
    d1l_contact_store_stats_t contact_stats = {0};
    uint32_t marker_generation = 0U;
    snapshot_admitted_identity(fingerprint, key, &node, &contact, &node_stats,
                               &contact_stats, &marker_generation);
    const d1l_meshcore_advert_admission_receipt_t receipt =
        admit_verified_advert(fingerprint, key, "Equal Replay", 100U, true,
                              45000000, -79000000);
    assert(receipt.outcome == D1L_MESHCORE_ADVERT_ADMISSION_REPLAY_REJECTED);
    assert_admitted_identity_unchanged(
        fingerprint, key, &node, &contact, node_stats, contact_stats,
        marker_generation);
}

/* d1l_older_timestamp_rejected_non_mutating */
static void test_d1l_older_timestamp_rejected_non_mutating(void)
{
    initialize_admission_stores();
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x33U);
    fingerprint_from_key(fingerprint, key);
    assert(admit_verified_advert(fingerprint, key, "Baseline", 100U, false,
                                 0, 0)
               .outcome == D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED);

    d1l_node_entry_t node = {0};
    d1l_contact_entry_t contact = {0};
    d1l_node_store_stats_t node_stats = {0};
    d1l_contact_store_stats_t contact_stats = {0};
    uint32_t marker_generation = 0U;
    snapshot_admitted_identity(fingerprint, key, &node, &contact, &node_stats,
                               &contact_stats, &marker_generation);
    const d1l_meshcore_advert_admission_receipt_t receipt =
        admit_verified_advert(fingerprint, key, "Older Replay", 99U, true,
                              45000000, -79000000);
    assert(receipt.outcome == D1L_MESHCORE_ADVERT_ADMISSION_REPLAY_REJECTED);
    assert_admitted_identity_unchanged(
        fingerprint, key, &node, &contact, node_stats, contact_stats,
        marker_generation);
}

/* d1l_strictly_newer_timestamp_accepted */
static void test_d1l_strictly_newer_timestamp_accepted(void)
{
    initialize_admission_stores();
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x34U);
    fingerprint_from_key(fingerprint, key);
    assert(admit_verified_advert(fingerprint, key, "Baseline", 100U, true,
                                 43427977, -80316478)
               .outcome == D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED);

    const d1l_meshcore_advert_admission_receipt_t receipt =
        admit_verified_advert(fingerprint, key, "Newer", 101U, false, 0, 0);
    assert(receipt.outcome == D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED);
    assert(receipt.contact_result == D1L_CONTACT_VERIFIED_ADVERT_UPDATED);
    assert(receipt.contact_store_error == ESP_OK);
    d1l_node_entry_t node = {0};
    d1l_contact_entry_t contact = {0};
    assert(d1l_node_store_find_by_fingerprint(fingerprint, &node));
    assert(d1l_contact_store_find_by_public_key(key, &contact));
    assert(node.advert_timestamp == 101U);
    assert(strcmp(node.name, "Newer") == 0);
    assert(node.location_valid);
    assert(node.lat_e6 == 43427977);
    assert(node.lon_e6 == -80316478);
    assert(node.location_advert_timestamp == 100U);
    assert(contact.signed_advert_timestamp == 101U);
}

/* d1l_uint32_max_then_zero_no_wrap */
static void test_d1l_uint32_max_then_zero_no_wrap(void)
{
    initialize_admission_stores();
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x35U);
    fingerprint_from_key(fingerprint, key);
    assert(admit_verified_advert(fingerprint, key, "Near Max", UINT32_MAX - 1U,
                                 false, 0, 0)
               .outcome == D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED);
    assert(admit_verified_advert(fingerprint, key, "At Max", UINT32_MAX,
                                 false, 0, 0)
               .outcome == D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED);

    d1l_node_entry_t node = {0};
    d1l_contact_entry_t contact = {0};
    d1l_node_store_stats_t node_stats = {0};
    d1l_contact_store_stats_t contact_stats = {0};
    uint32_t marker_generation = 0U;
    snapshot_admitted_identity(fingerprint, key, &node, &contact, &node_stats,
                               &contact_stats, &marker_generation);
    const d1l_meshcore_advert_admission_receipt_t receipt =
        admit_verified_advert(fingerprint, key, "Wrapped Zero", 0U, false, 0, 0);
    assert(receipt.outcome == D1L_MESHCORE_ADVERT_ADMISSION_REPLAY_REJECTED);
    assert(node.advert_timestamp == UINT32_MAX);
    assert(contact.signed_advert_timestamp == UINT32_MAX);
    assert_admitted_identity_unchanged(
        fingerprint, key, &node, &contact, node_stats, contact_stats,
        marker_generation);
}

/* d1l_equal_exact_key_contact_retry */
static void test_d1l_equal_exact_key_contact_retry(void)
{
    initialize_admission_stores();
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x36U);
    fingerprint_from_key(fingerprint, key);
    bool stale = true;
    assert(d1l_node_store_upsert_advert(
               fingerprint, key, "Retry Peer", 'C', -55, 80, 1U, 2U,
               300U, false, 0, 0, &stale) == ESP_OK);
    assert(!stale);
    assert(!d1l_contact_store_find_by_public_key(key, NULL));

    mock_nvs_fail_next_set(ESP_FAIL);
    d1l_meshcore_advert_admission_receipt_t receipt =
        admit_verified_advert(fingerprint, key, "Retry Peer", 300U, false, 0, 0);
    assert(receipt.outcome ==
           D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_FAILED);
    assert(receipt.contact_store_error == ESP_FAIL);
    assert(receipt.contact_result == D1L_CONTACT_VERIFIED_ADVERT_NONE);
    assert(!d1l_contact_store_find_by_public_key(key, NULL));

    receipt = admit_verified_advert(fingerprint, key, "Retry Peer", 300U,
                                    false, 0, 0);
    assert(receipt.outcome ==
           D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_SUCCEEDED);
    assert(receipt.contact_store_error == ESP_OK);
    assert(receipt.contact_result == D1L_CONTACT_VERIFIED_ADVERT_CREATED);
    d1l_contact_entry_t contact = {0};
    assert(d1l_contact_store_find_by_public_key(key, &contact));
    assert(contact.signed_advert_timestamp == 300U);

    receipt = admit_verified_advert(fingerprint, key, "Retry Peer", 300U,
                                    false, 0, 0);
    assert(receipt.outcome == D1L_MESHCORE_ADVERT_ADMISSION_REPLAY_REJECTED);
}

/* d1l_full_key_prefix_collision_precedes_replay */
static void test_d1l_full_key_prefix_collision_precedes_replay(void)
{
    initialize_admission_stores();
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x37U);
    fingerprint_from_key(fingerprint, key);
    assert(admit_verified_advert(fingerprint, key, "Baseline", 400U, false,
                                 0, 0)
               .outcome == D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED);

    d1l_node_entry_t node = {0};
    d1l_contact_entry_t contact = {0};
    d1l_node_store_stats_t node_stats = {0};
    d1l_contact_store_stats_t contact_stats = {0};
    uint32_t marker_generation = 0U;
    snapshot_admitted_identity(fingerprint, key, &node, &contact, &node_stats,
                               &contact_stats, &marker_generation);
    char colliding_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    copy_field(colliding_key, sizeof(colliding_key), key);
    colliding_key[D1L_NODE_PUBLIC_KEY_HEX_LEN - 2U] =
        colliding_key[D1L_NODE_PUBLIC_KEY_HEX_LEN - 2U] == 'f' ? 'e' : 'f';

    d1l_meshcore_advert_admission_receipt_t receipt =
        admit_verified_advert(fingerprint, colliding_key, "Equal Collision",
                              400U, false, 0, 0);
    assert(receipt.outcome == D1L_MESHCORE_ADVERT_ADMISSION_KEY_COLLISION);
    assert(receipt.node_store_error == ESP_ERR_INVALID_STATE);
    assert_admitted_identity_unchanged(
        fingerprint, key, &node, &contact, node_stats, contact_stats,
        marker_generation);

    receipt = admit_verified_advert(fingerprint, colliding_key,
                                    "Newer Collision", 401U, true,
                                    45000000, -79000000);
    assert(receipt.outcome == D1L_MESHCORE_ADVERT_ADMISSION_KEY_COLLISION);
    assert(receipt.node_store_error == ESP_ERR_INVALID_STATE);
    assert_admitted_identity_unchanged(
        fingerprint, key, &node, &contact, node_stats, contact_stats,
        marker_generation);
}

static d1l_node_entry_t make_heard_node(const char *name, const char *type)
{
    d1l_node_entry_t node = {0};
    copy_field(node.public_key_hex, sizeof(node.public_key_hex), KEY_HEX);
    copy_field(node.name, sizeof(node.name), name);
    copy_field(node.type, sizeof(node.type), type);
    node.rssi_dbm = -61;
    node.snr_tenths = 72;
    node.path_hash_bytes = 1U;
    node.path_hops = 2U;
    return node;
}

static void make_public_key(char key[D1L_NODE_PUBLIC_KEY_HEX_LEN], uint32_t id)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U; ++i) {
        key[i] = hex[(i + id) & 0x0fU];
    }
    for (size_t i = 0; i < 8U; ++i) {
        key[15U - i] = hex[(id >> (i * 4U)) & 0x0fU];
    }
    key[D1L_NODE_PUBLIC_KEY_HEX_LEN - 1U] = '\0';
}

static void fingerprint_from_key(
    char fingerprint[D1L_NODE_FINGERPRINT_LEN],
    const char key[D1L_NODE_PUBLIC_KEY_HEX_LEN])
{
    memcpy(fingerprint, key, D1L_NODE_FINGERPRINT_LEN - 1U);
    fingerprint[D1L_NODE_FINGERPRINT_LEN - 1U] = '\0';
}

static void uppercase_hex(char *dest, size_t dest_size, const char *src)
{
    size_t i = 0U;
    for (; i + 1U < dest_size && src[i] != '\0'; ++i) {
        dest[i] = (src[i] >= 'a' && src[i] <= 'f')
                      ? (char)(src[i] - ('a' - 'A'))
                      : src[i];
    }
    dest[i] = '\0';
}

static d1l_node_entry_t make_verified_node(const char *public_key_hex,
                                           const char *name,
                                           const char *type)
{
    d1l_node_entry_t node = make_heard_node(name, type);
    copy_field(node.public_key_hex, sizeof(node.public_key_hex), public_key_hex);
    node.advert_timestamp = 123U;
    node.last_heard_ms = 456U;
    return node;
}

static void calculate_advert_hash(
    uint8_t variant, uint8_t hash[D1L_MESHCORE_PACKET_HASH_BYTES])
{
    const uint8_t payload[] = {0x41U, variant, 0x99U};
    const d1l_meshcore_wire_packet_t packet = {
        .header = (uint8_t)((D1L_MESHCORE_PAYLOAD_ADVERT << 2U) |
                           D1L_MESHCORE_ROUTE_FLOOD),
        .route = D1L_MESHCORE_ROUTE_FLOOD,
        .type = D1L_MESHCORE_PAYLOAD_ADVERT,
        .version = D1L_MESHCORE_PAYLOAD_VER_1,
        .path_len = 0U,
        .path_hash_bytes = 1U,
        .payload = payload,
        .payload_len = sizeof(payload),
    };
    assert(d1l_meshcore_packet_hash_calculate(&packet, hash) == ESP_OK);
}

/* d1l_advert_accepted_terminal_hash_remembered */
static void test_d1l_advert_accepted_terminal_hash_remembered(void)
{
    initialize_admission_stores();
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x41U);
    fingerprint_from_key(fingerprint, key);
    const d1l_meshcore_advert_admission_receipt_t receipt =
        admit_verified_advert(fingerprint, key, "Terminal", 500U, false, 0, 0);
    assert(receipt.outcome == D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED);
    assert(d1l_meshcore_advert_admission_receipt_cacheable(&receipt));

    d1l_meshcore_packet_hash_cache_t cache = {0};
    uint8_t hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
    calculate_advert_hash(1U, hash);
    assert(d1l_meshcore_packet_hash_cache_remember(&cache, hash));
    assert(d1l_meshcore_packet_hash_cache_contains(&cache, hash));
}

/* d1l_distinct_equal_timestamp_hash_miss_reaches_replay */
static void test_d1l_distinct_equal_timestamp_hash_miss_reaches_replay(void)
{
    initialize_admission_stores();
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x42U);
    fingerprint_from_key(fingerprint, key);
    const d1l_meshcore_advert_admission_receipt_t accepted =
        admit_verified_advert(fingerprint, key, "Baseline", 600U, false, 0, 0);
    assert(d1l_meshcore_advert_admission_receipt_cacheable(&accepted));

    d1l_meshcore_packet_hash_cache_t cache = {0};
    uint8_t first_hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
    uint8_t distinct_hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
    calculate_advert_hash(2U, first_hash);
    calculate_advert_hash(3U, distinct_hash);
    assert(d1l_meshcore_packet_hash_cache_remember(&cache, first_hash));
    assert(!d1l_meshcore_packet_hash_cache_contains(&cache, distinct_hash));

    const d1l_meshcore_advert_admission_receipt_t replay =
        admit_verified_advert(fingerprint, key, "Distinct", 600U, false, 0, 0);
    assert(replay.outcome == D1L_MESHCORE_ADVERT_ADMISSION_REPLAY_REJECTED);
    assert(d1l_meshcore_advert_admission_receipt_cacheable(&replay));
    assert(d1l_meshcore_packet_hash_cache_remember(&cache, distinct_hash));
}

/* d1l_contact_retry_failure_hash_not_remembered */
static void test_d1l_contact_retry_failure_hash_not_remembered(void)
{
    initialize_admission_stores();
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x43U);
    fingerprint_from_key(fingerprint, key);
    bool stale = true;
    assert(d1l_node_store_upsert_advert(
               fingerprint, key, "Retry", 'C', -55, 80, 1U, 2U, 700U,
               false, 0, 0, &stale) == ESP_OK);
    assert(!stale);

    mock_nvs_fail_next_set(ESP_FAIL);
    const d1l_meshcore_advert_admission_receipt_t failed =
        admit_verified_advert(fingerprint, key, "Retry", 700U, false, 0, 0);
    assert(failed.outcome ==
           D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_FAILED);
    assert(!d1l_meshcore_advert_admission_receipt_cacheable(&failed));
    d1l_meshcore_packet_hash_cache_t cache = {0};
    uint8_t hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
    calculate_advert_hash(4U, hash);
    assert(!d1l_meshcore_packet_hash_cache_contains(&cache, hash));
}

/* d1l_accepted_contact_failure_hash_not_remembered */
static void test_d1l_accepted_contact_failure_hash_not_remembered(void)
{
    initialize_admission_stores();
    for (uint32_t id = 100U;
         id < 100U + D1L_CONTACT_STORE_CAPACITY; ++id) {
        char existing_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
        char existing_fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
        make_public_key(existing_key, id);
        fingerprint_from_key(existing_fingerprint, existing_key);
        d1l_node_entry_t node =
            make_verified_node(existing_key, "Full", "chat");
        d1l_contact_verified_advert_result_t result =
            D1L_CONTACT_VERIFIED_ADVERT_NONE;
        assert(d1l_contact_store_upsert_verified_advert(
                   existing_fingerprint, &node, &result, NULL) == ESP_OK);
    }

    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x90U);
    fingerprint_from_key(fingerprint, key);
    const d1l_meshcore_advert_admission_receipt_t failed =
        admit_verified_advert(fingerprint, key, "No Space", 800U, false, 0, 0);
    assert(failed.outcome == D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED);
    assert(failed.contact_store_error == ESP_ERR_NO_MEM);
    assert(!d1l_meshcore_advert_admission_receipt_cacheable(&failed));
    d1l_meshcore_packet_hash_cache_t cache = {0};
    uint8_t hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
    calculate_advert_hash(5U, hash);
    assert(!d1l_meshcore_packet_hash_cache_contains(&cache, hash));
}

/* d1l_contact_retry_success_hash_remembered */
static void test_d1l_contact_retry_success_hash_remembered(void)
{
    initialize_admission_stores();
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x44U);
    fingerprint_from_key(fingerprint, key);
    bool stale = true;
    assert(d1l_node_store_upsert_advert(
               fingerprint, key, "Retry", 'C', -55, 80, 1U, 2U, 900U,
               false, 0, 0, &stale) == ESP_OK);
    assert(!stale);

    const d1l_meshcore_advert_admission_receipt_t succeeded =
        admit_verified_advert(fingerprint, key, "Retry", 900U, false, 0, 0);
    assert(succeeded.outcome ==
           D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_SUCCEEDED);
    assert(d1l_meshcore_advert_admission_receipt_cacheable(&succeeded));
    d1l_meshcore_packet_hash_cache_t cache = {0};
    uint8_t hash[D1L_MESHCORE_PACKET_HASH_BYTES] = {0};
    calculate_advert_hash(6U, hash);
    assert(d1l_meshcore_packet_hash_cache_remember(&cache, hash));
    assert(d1l_meshcore_packet_hash_cache_contains(&cache, hash));
}

static void test_verified_contact_create_reload_update_preserves_preferences(void)
{
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 1U);
    fingerprint_from_key(fingerprint, key);
    d1l_node_entry_t node = make_verified_node(key, "Verified One", "chat");
    d1l_contact_verified_advert_result_t result =
        D1L_CONTACT_VERIFIED_ADVERT_NONE;
    d1l_contact_entry_t contact = {0};
    assert(d1l_contact_store_upsert_verified_advert(
               fingerprint, &node, &result, &contact) == ESP_OK);
    assert(result == D1L_CONTACT_VERIFIED_ADVERT_CREATED);
    assert(strcmp(contact.public_key_hex, key) == 0);
    assert(strcmp(contact.alias, "Verified One") == 0);
    assert(contact.verification_source ==
           D1L_CONTACT_VERIFICATION_SIGNED_ADVERT);
    assert(contact.signed_advert_timestamp == 123U);
    assert(contact.last_heard_ms == 456U);
    assert(d1l_contact_store_is_canonical(&contact));
    assert(d1l_contact_store_can_dm(&contact));

    assert(d1l_contact_store_rename(fingerprint, "Pinned Alias", NULL) == ESP_OK);
    assert(d1l_contact_store_set_flags(fingerprint, true, true, NULL) == ESP_OK);
    const uint8_t path[] = {0xa5U, 0x5aU};
    assert(d1l_contact_store_update_path(fingerprint, path, 2U) == ESP_OK);
    d1l_contact_entry_t preferred = {0};
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &preferred));
    assert(d1l_contact_store_flush() == ESP_OK);

    assert(d1l_contact_store_init() == ESP_OK);
    char upper_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char upper_fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    uppercase_hex(upper_key, sizeof(upper_key), key);
    uppercase_hex(upper_fingerprint, sizeof(upper_fingerprint), fingerprint);
    node = make_verified_node(upper_key, "Fresh Heard Name", "repeater");
    node.rssi_dbm = -44;
    result = D1L_CONTACT_VERIFIED_ADVERT_NONE;
    memset(&contact, 0, sizeof(contact));
    assert(d1l_contact_store_upsert_verified_advert(
               upper_fingerprint, &node, &result, &contact) == ESP_OK);
    assert(result == D1L_CONTACT_VERIFIED_ADVERT_UPDATED);
    assert(strcmp(contact.public_key_hex, key) == 0);
    assert(strcmp(contact.fingerprint, fingerprint) == 0);
    assert(strcmp(contact.alias, preferred.alias) == 0);
    assert(contact.favorite == preferred.favorite);
    assert(contact.muted == preferred.muted);
    assert(contact.out_path_valid == preferred.out_path_valid);
    assert(contact.out_path_len == preferred.out_path_len);
    assert(memcmp(contact.out_path, preferred.out_path,
                  sizeof(contact.out_path)) == 0);
    assert(strcmp(contact.heard_name, "Fresh Heard Name") == 0);
    assert(strcmp(contact.type, "repeater") == 0);
    assert(contact.last_rssi_dbm == -44);
    assert(d1l_contact_store_find_by_public_key(upper_key, &contact));
    assert(strcmp(contact.alias, "Pinned Alias") == 0);
    assert(d1l_contact_store_stats().count == 1U);
}

static void test_verified_contact_promotes_placeholder_without_losing_preferences(void)
{
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 2U);
    fingerprint_from_key(fingerprint, key);
    assert(d1l_contact_store_upsert_from_node(
               fingerprint, "Manual Alias", NULL) == ESP_OK);
    assert(d1l_contact_store_set_flags(fingerprint, true, true, NULL) == ESP_OK);
    const uint8_t path[] = {0x11U, 0x22U, 0x33U};
    assert(d1l_contact_store_update_path(fingerprint, path, 3U) == ESP_OK);

    d1l_node_entry_t node = make_verified_node(key, "Signed Name", "room");
    d1l_contact_verified_advert_result_t result =
        D1L_CONTACT_VERIFIED_ADVERT_NONE;
    d1l_contact_entry_t contact = {0};
    assert(d1l_contact_store_upsert_verified_advert(
               fingerprint, &node, &result, &contact) == ESP_OK);
    assert(result == D1L_CONTACT_VERIFIED_ADVERT_PROMOTED_PLACEHOLDER);
    assert(strcmp(contact.public_key_hex, key) == 0);
    assert(strcmp(contact.alias, "Manual Alias") == 0);
    assert(contact.favorite && contact.muted);
    assert(contact.out_path_valid && contact.out_path_len == 3U);
    assert(memcmp(contact.out_path, path, sizeof(path)) == 0);
    assert(strcmp(contact.heard_name, "Signed Name") == 0);
    assert(d1l_contact_store_stats().count == 1U);
}

static void test_verified_contact_refuses_collision_and_rolls_back_nvs_failure(void)
{
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 3U);
    fingerprint_from_key(fingerprint, key);
    d1l_node_entry_t node = make_verified_node(key, "Original", "chat");
    d1l_contact_verified_advert_result_t result =
        D1L_CONTACT_VERIFIED_ADVERT_NONE;
    assert(d1l_contact_store_upsert_verified_advert(
               fingerprint, &node, &result, NULL) == ESP_OK);

    d1l_contact_entry_t baseline = {0};
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &baseline));
    const d1l_contact_store_stats_t baseline_stats = d1l_contact_store_stats();
    node = make_verified_node(key, "Failed Update", "repeater");
    mock_nvs_fail_next_set(ESP_FAIL);
    result = D1L_CONTACT_VERIFIED_ADVERT_CREATED;
    assert(d1l_contact_store_upsert_verified_advert(
               fingerprint, &node, &result, NULL) == ESP_FAIL);
    assert(result == D1L_CONTACT_VERIFIED_ADVERT_NONE);
    d1l_contact_entry_t after = {0};
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &after));
    assert(memcmp(&after, &baseline, sizeof(after)) == 0);
    assert_contact_stats_equal(d1l_contact_store_stats(), baseline_stats);
    assert(d1l_contact_store_init() == ESP_OK);
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &after));
    assert(memcmp(&after, &baseline, sizeof(after)) == 0);

    char colliding_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    copy_field(colliding_key, sizeof(colliding_key), key);
    colliding_key[D1L_NODE_PUBLIC_KEY_HEX_LEN - 2U] =
        colliding_key[D1L_NODE_PUBLIC_KEY_HEX_LEN - 2U] == 'f' ? 'e' : 'f';
    node = make_verified_node(colliding_key, "Collision", "room");
    d1l_contact_entry_t rejected = baseline;
    result = D1L_CONTACT_VERIFIED_ADVERT_NONE;
    assert(d1l_contact_store_upsert_verified_advert(
               fingerprint, &node, &result, &rejected) == ESP_ERR_INVALID_STATE);
    assert(result == D1L_CONTACT_VERIFIED_ADVERT_COLLISION);
    memset(&after, 0, sizeof(after));
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &after));
    assert(memcmp(&after, &baseline, sizeof(after)) == 0);
    assert_contact_stats_equal(d1l_contact_store_stats(), baseline_stats);
    d1l_contact_entry_t empty = {0};
    assert(memcmp(&rejected, &empty, sizeof(rejected)) == 0);

    char retry_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char retry_fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(retry_key, 4U);
    fingerprint_from_key(retry_fingerprint, retry_key);
    node = make_verified_node(retry_key, "Retry", "sensor");
    mock_nvs_fail_next_set(ESP_FAIL);
    result = D1L_CONTACT_VERIFIED_ADVERT_UPDATED;
    assert(d1l_contact_store_upsert_verified_advert(
               retry_fingerprint, &node, &result, NULL) == ESP_FAIL);
    assert(result == D1L_CONTACT_VERIFIED_ADVERT_NONE);
    assert(!d1l_contact_store_find_by_public_key(retry_key, NULL));
    assert_contact_stats_equal(d1l_contact_store_stats(), baseline_stats);
    assert(d1l_contact_store_init() == ESP_OK);
    result = D1L_CONTACT_VERIFIED_ADVERT_NONE;
    assert(d1l_contact_store_upsert_verified_advert(
               retry_fingerprint, &node, &result, NULL) == ESP_OK);
    assert(result == D1L_CONTACT_VERIFIED_ADVERT_CREATED);
}

static void test_verified_contact_full_store_never_evicts(void)
{
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);
    char first_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char first_fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    for (uint32_t id = 20U;
         id < 20U + D1L_CONTACT_STORE_CAPACITY; ++id) {
        char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
        char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
        make_public_key(key, id);
        fingerprint_from_key(fingerprint, key);
        if (id == 20U) {
            copy_field(first_key, sizeof(first_key), key);
            copy_field(first_fingerprint, sizeof(first_fingerprint), fingerprint);
        }
        d1l_node_entry_t node = make_verified_node(key, "Capacity", "chat");
        d1l_contact_verified_advert_result_t result =
            D1L_CONTACT_VERIFIED_ADVERT_NONE;
        assert(d1l_contact_store_upsert_verified_advert(
                   fingerprint, &node, &result, NULL) == ESP_OK);
        assert(result == D1L_CONTACT_VERIFIED_ADVERT_CREATED);
    }
    const d1l_contact_store_stats_t full_stats = d1l_contact_store_stats();
    assert(full_stats.count == D1L_CONTACT_STORE_CAPACITY);
    assert(full_stats.dropped_oldest == 0U);

    char overflow_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char overflow_fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(overflow_key, 99U);
    fingerprint_from_key(overflow_fingerprint, overflow_key);
    d1l_node_entry_t overflow = make_verified_node(
        overflow_key, "Overflow", "chat");
    d1l_contact_verified_advert_result_t result =
        D1L_CONTACT_VERIFIED_ADVERT_NONE;
    assert(d1l_contact_store_upsert_verified_advert(
               overflow_fingerprint, &overflow, &result, NULL) == ESP_ERR_NO_MEM);
    assert(result == D1L_CONTACT_VERIFIED_ADVERT_FULL);
    assert_contact_stats_equal(d1l_contact_store_stats(), full_stats);
    assert(d1l_contact_store_find_by_public_key(first_key, NULL));
    assert(d1l_contact_store_find_by_fingerprint(first_fingerprint, NULL));
    assert(!d1l_contact_store_find_by_public_key(overflow_key, NULL));
    d1l_node_entry_t heard = make_heard_node("Bypass", "chat");
    assert(d1l_contact_store_upsert_from_node(
               "ffffffffffffffff", "Bypass", &heard) == ESP_ERR_NO_MEM);
    assert_contact_stats_equal(d1l_contact_store_stats(), full_stats);
    assert(d1l_contact_store_find_by_public_key(first_key, NULL));
}

static void test_favorite_placeholders_are_never_evicted(void)
{
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);
    char first_fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    for (uint32_t i = 0U; i < D1L_CONTACT_STORE_CAPACITY; ++i) {
        char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
        (void)snprintf(fingerprint, sizeof(fingerprint), "%016llx",
                       (unsigned long long)(0x1000ULL + i));
        if (i == 0U) {
            copy_field(first_fingerprint, sizeof(first_fingerprint), fingerprint);
        }
        assert(d1l_contact_store_upsert_from_node(
                   fingerprint, "Favorite", NULL) == ESP_OK);
        assert(d1l_contact_store_set_flags(fingerprint, true, false, NULL) ==
               ESP_OK);
    }
    const d1l_contact_store_stats_t full = d1l_contact_store_stats();
    assert(full.count == D1L_CONTACT_STORE_CAPACITY);
    assert(d1l_contact_store_upsert_from_node(
               "eeeeeeeeeeeeeeee", "Overflow", NULL) == ESP_ERR_NO_MEM);
    assert_contact_stats_equal(d1l_contact_store_stats(), full);
    assert(d1l_contact_store_find_by_fingerprint(first_fingerprint, NULL));
}

static void test_contact_nvs_failure_rolls_back_and_retry_succeeds(void)
{
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);
    d1l_node_entry_t heard = make_heard_node("Original", "chat");
    assert(d1l_contact_store_upsert_from_node("aaaaaaaaaaaaaaaa", "Original",
                                               &heard) == ESP_OK);
    d1l_contact_entry_t baseline = {0};
    assert(d1l_contact_store_find_by_fingerprint("aaaaaaaaaaaaaaaa", &baseline));
    const d1l_contact_store_stats_t baseline_stats = d1l_contact_store_stats();

    mock_timer_set_us(4000000);
    mock_nvs_fail_next_set(ESP_FAIL);
    assert(d1l_contact_store_rename("aaaaaaaaaaaaaaaa", "Changed", NULL) ==
           ESP_FAIL);
    d1l_contact_entry_t after = {0};
    assert(d1l_contact_store_find_by_fingerprint("aaaaaaaaaaaaaaaa", &after));
    assert(memcmp(&after, &baseline, sizeof(after)) == 0);
    assert_contact_stats_equal(d1l_contact_store_stats(), baseline_stats);

    assert(d1l_contact_store_init() == ESP_OK);
    assert(d1l_contact_store_find_by_fingerprint("aaaaaaaaaaaaaaaa", &after));
    assert(memcmp(&after, &baseline, sizeof(after)) == 0);
    assert(d1l_contact_store_rename("aaaaaaaaaaaaaaaa", "Changed", &after) ==
           ESP_OK);
    assert(strcmp(after.alias, "Changed") == 0);

    const d1l_contact_store_stats_t before_new = d1l_contact_store_stats();
    d1l_node_entry_t second = make_heard_node("Second", "room");
    mock_nvs_fail_next_set(ESP_FAIL);
    assert(d1l_contact_store_upsert_from_node("bbbbbbbbbbbbbbbb", "Second",
                                               &second) == ESP_FAIL);
    assert(!d1l_contact_store_find_by_fingerprint("bbbbbbbbbbbbbbbb", NULL));
    assert_contact_stats_equal(d1l_contact_store_stats(), before_new);
    assert(d1l_contact_store_upsert_from_node("bbbbbbbbbbbbbbbb", "Second",
                                               &second) == ESP_OK);
    assert(d1l_contact_store_find_by_fingerprint("bbbbbbbbbbbbbbbb", &after));
    assert(strcmp(after.type, "room") == 0);
}

static void make_contact_uri(char out[D1L_CONTACT_EXPORT_URI_LEN],
                             const char *name, const char *key,
                             unsigned type_id)
{
    const int written = snprintf(
        out, D1L_CONTACT_EXPORT_URI_LEN,
        "meshcore://contact/add?name=%s&public_key=%s&type=%u", name, key,
        type_id);
    assert(written > 0 && written < (int)D1L_CONTACT_EXPORT_URI_LEN);
}

static void test_prepare_after_concurrent_delete_fails_closed(void)
{
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    char uri[D1L_CONTACT_EXPORT_URI_LEN] = {0};
    make_public_key(key, 0x60U);
    fingerprint_from_key(fingerprint, key);
    make_contact_uri(uri, "DeleteRace", key, 1U);
    d1l_contact_import_result_t import_result = D1L_CONTACT_IMPORT_NONE;
    d1l_contact_entry_t stale = {0};
    assert(d1l_contact_store_import_uri(
               uri, strlen(uri), &import_result, &stale) == ESP_OK);
    assert(d1l_contact_store_can_dm(&stale));

    assert(d1l_contact_store_delete(fingerprint, NULL) == ESP_OK);
    d1l_contact_entry_t prepared = {0};
    bool expired = false;
    assert(d1l_contact_store_prepare_path_route(
               stale.fingerprint, 1000U, &prepared, &expired) ==
           ESP_ERR_NOT_FOUND);
    assert(d1l_contact_store_stats().count == 0U);
}

static void test_uri_import_promotes_placeholder_and_preserves_preferences(void)
{
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x61U);
    fingerprint_from_key(fingerprint, key);

    d1l_node_entry_t heard = make_verified_node(key, "Observed Name", "chat");
    assert(d1l_contact_store_upsert_from_node(
               fingerprint, "Pinned Alias", &heard) == ESP_OK);
    assert(d1l_contact_store_set_flags(fingerprint, true, true, NULL) == ESP_OK);
    const uint8_t path[] = {0x11U, 0x22U};
    assert(d1l_contact_store_update_path(fingerprint, path, 2U) == ESP_OK);

    d1l_contact_entry_t contact = {0};
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &contact));
    assert(contact.public_key_hex[0] == '\0');
    assert(contact.verification_source == D1L_CONTACT_VERIFICATION_NONE);
    assert(!d1l_contact_store_is_canonical(&contact));
    assert(!d1l_contact_store_can_dm(&contact));
    char exported[D1L_CONTACT_EXPORT_URI_LEN] = "dirty";
    assert(d1l_contact_store_export_uri(&contact, exported,
                                        sizeof(exported)) ==
           ESP_ERR_INVALID_STATE);
    assert(exported[0] == '\0');

    char uri[D1L_CONTACT_EXPORT_URI_LEN] = {0};
    make_contact_uri(uri, "Imported+Name", key, 1U);
    d1l_contact_import_result_t result = D1L_CONTACT_IMPORT_NONE;
    assert(d1l_contact_store_import_uri(uri, strlen(uri), &result, &contact) ==
           ESP_OK);
    assert(result == D1L_CONTACT_IMPORT_PROMOTED_PLACEHOLDER);
    assert(strcmp(contact.public_key_hex, key) == 0);
    assert(strcmp(contact.alias, "Pinned Alias") == 0);
    assert(strcmp(contact.type, "chat") == 0);
    assert(contact.favorite && contact.muted);
    assert(contact.out_path_valid && contact.out_path_len == 2U);
    assert(memcmp(contact.out_path, path, sizeof(path)) == 0);
    assert(contact.verification_source ==
           D1L_CONTACT_VERIFICATION_URI_IMPORT);
    assert(contact.signed_advert_timestamp == 0U);
    assert(contact.last_heard_ms == 0U);
    assert(d1l_contact_store_is_canonical(&contact));
    assert(d1l_contact_store_can_dm(&contact));
    assert(!d1l_contact_store_can_admin(&contact));

    assert(d1l_contact_store_init() == ESP_OK);
    assert(d1l_contact_store_find_by_public_key(key, &contact));
    assert(contact.verification_source ==
           D1L_CONTACT_VERIFICATION_URI_IMPORT);
    assert(strcmp(contact.alias, "Pinned Alias") == 0);

    const d1l_contact_store_stats_t before_duplicate =
        d1l_contact_store_stats();
    result = D1L_CONTACT_IMPORT_NONE;
    assert(d1l_contact_store_import_uri(uri, strlen(uri), &result, &contact) ==
           ESP_OK);
    assert(result == D1L_CONTACT_IMPORT_UPDATED);
    assert(d1l_contact_store_stats().count == 1U);
    assert_contact_stats_equal(d1l_contact_store_stats(), before_duplicate);
    assert(strcmp(contact.alias, "Pinned Alias") == 0);
}

static void test_uri_import_conflicts_and_nvs_failure_are_non_mutating(void)
{
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);
    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    make_public_key(key, 0x71U);
    fingerprint_from_key(fingerprint, key);
    char uri[D1L_CONTACT_EXPORT_URI_LEN] = {0};
    make_contact_uri(uri, "Canonical", key, 2U);

    d1l_contact_import_result_t result = D1L_CONTACT_IMPORT_NONE;
    d1l_contact_entry_t baseline = {0};
    assert(d1l_contact_store_import_uri(uri, strlen(uri), &result, &baseline) ==
           ESP_OK);
    assert(result == D1L_CONTACT_IMPORT_CREATED);
    assert(d1l_contact_store_can_admin(&baseline));
    const d1l_contact_store_stats_t baseline_stats = d1l_contact_store_stats();

    make_contact_uri(uri, "Conflicting", key, 1U);
    result = D1L_CONTACT_IMPORT_NONE;
    assert(d1l_contact_store_import_uri(uri, strlen(uri), &result, NULL) ==
           ESP_ERR_INVALID_STATE);
    assert(result == D1L_CONTACT_IMPORT_ROLE_CONFLICT);
    d1l_contact_entry_t after = {0};
    assert(d1l_contact_store_find_by_fingerprint(fingerprint, &after));
    assert(memcmp(&after, &baseline, sizeof(after)) == 0);
    assert_contact_stats_equal(d1l_contact_store_stats(), baseline_stats);

    char colliding_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    copy_field(colliding_key, sizeof(colliding_key), key);
    colliding_key[D1L_NODE_PUBLIC_KEY_HEX_LEN - 2U] =
        colliding_key[D1L_NODE_PUBLIC_KEY_HEX_LEN - 2U] == 'f' ? 'e' : 'f';
    make_contact_uri(uri, "Collision", colliding_key, 2U);
    result = D1L_CONTACT_IMPORT_NONE;
    assert(d1l_contact_store_import_uri(uri, strlen(uri), &result, NULL) ==
           ESP_ERR_INVALID_STATE);
    assert(result == D1L_CONTACT_IMPORT_COLLISION);
    assert_contact_stats_equal(d1l_contact_store_stats(), baseline_stats);

    char retry_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    make_public_key(retry_key, 0x72U);
    make_contact_uri(uri, "Retry", retry_key, 4U);
    mock_nvs_fail_next_set(ESP_FAIL);
    result = D1L_CONTACT_IMPORT_CREATED;
    assert(d1l_contact_store_import_uri(uri, strlen(uri), &result, NULL) ==
           ESP_FAIL);
    assert(result == D1L_CONTACT_IMPORT_NONE);
    assert(!d1l_contact_store_find_by_public_key(retry_key, NULL));
    assert_contact_stats_equal(d1l_contact_store_stats(), baseline_stats);
    assert(d1l_contact_store_init() == ESP_OK);
    result = D1L_CONTACT_IMPORT_NONE;
    assert(d1l_contact_store_import_uri(uri, strlen(uri), &result, &after) ==
           ESP_OK);
    assert(result == D1L_CONTACT_IMPORT_CREATED);
    assert(strcmp(after.type, "sensor") == 0);
}

static void test_uri_import_preserves_official_names(void)
{
    mock_nvs_reset();
    assert(d1l_contact_store_init() == ESP_OK);

    char key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    make_public_key(key, 0x81U);
    char uri[D1L_CONTACT_EXPORT_URI_LEN] = {0};
    make_contact_uri(uri, "1234567890123456789012345678901", key, 1U);
    d1l_contact_import_result_t result = D1L_CONTACT_IMPORT_NONE;
    d1l_contact_entry_t contact = {0};
    assert(d1l_contact_store_import_uri(uri, strlen(uri), &result, &contact) ==
           ESP_OK);
    assert(result == D1L_CONTACT_IMPORT_CREATED);
    assert(strcmp(contact.alias, "1234567890123456789012345678901") == 0);
    char exported[D1L_CONTACT_EXPORT_URI_LEN] = {0};
    assert(d1l_contact_store_export_uri(&contact, exported,
                                        sizeof(exported)) == ESP_OK);
    assert(strcmp(exported, uri) == 0);

    make_public_key(key, 0x82U);
    make_contact_uri(uri, "Caf%C3%A9+%E6%9D%B1%E4%BA%AC", key, 1U);
    result = D1L_CONTACT_IMPORT_NONE;
    memset(&contact, 0, sizeof(contact));
    assert(d1l_contact_store_import_uri(uri, strlen(uri), &result, &contact) ==
           ESP_OK);
    assert(strcmp(contact.alias,
                  "Caf\xc3\xa9 \xe6\x9d\xb1\xe4\xba\xac") == 0);
    memset(exported, 0, sizeof(exported));
    assert(d1l_contact_store_export_uri(&contact, exported,
                                        sizeof(exported)) == ESP_OK);
    assert(strcmp(exported, uri) == 0);

    make_public_key(key, 0x83U);
    make_contact_uri(uri,
                     "A%22%2C%22x%22%3Atrue%2C%22s%22%3A%22%5CZ",
                     key, 1U);
    result = D1L_CONTACT_IMPORT_NONE;
    memset(&contact, 0, sizeof(contact));
    assert(d1l_contact_store_import_uri(uri, strlen(uri), &result, &contact) ==
           ESP_OK);
    assert(strcmp(contact.alias, "A\",\"x\":true,\"s\":\"\\Z") == 0);
    memset(exported, 0, sizeof(exported));
    assert(d1l_contact_store_export_uri(&contact, exported,
                                        sizeof(exported)) == ESP_OK);
    assert(strcmp(exported, uri) == 0);
}

static void test_full_key_chat_without_provenance_is_not_dm_capable(void)
{
    d1l_contact_entry_t contact = {0};
    copy_field(contact.fingerprint, sizeof(contact.fingerprint),
               "0123456789abcdef");
    copy_field(contact.public_key_hex, sizeof(contact.public_key_hex), KEY_HEX);
    copy_field(contact.alias, sizeof(contact.alias), "Unverified");
    copy_field(contact.type, sizeof(contact.type), "chat");
    contact.verification_source = D1L_CONTACT_VERIFICATION_NONE;

    assert(d1l_contact_store_has_export_key(&contact));
    assert(!d1l_contact_store_is_canonical(&contact));
    assert(!d1l_contact_store_can_dm(&contact));
    assert(!d1l_contact_store_can_admin(&contact));
    char uri[D1L_CONTACT_EXPORT_URI_LEN] = "dirty";
    assert(d1l_contact_store_export_uri(&contact, uri, sizeof(uri)) ==
           ESP_ERR_INVALID_STATE);
    assert(uri[0] == '\0');
}

static void test_path_probe_roles_do_not_expand_dm_capability(void)
{
    static const struct {
        const char *type;
        bool can_dm;
        bool can_path_probe;
        bool can_admin;
    } cases[] = {
        {"chat", true, true, false},
        {"repeater", false, true, true},
        {"room", false, true, true},
        {"sensor", false, true, false},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        d1l_contact_entry_t contact = {0};
        copy_field(contact.fingerprint, sizeof(contact.fingerprint),
                   "0123456789abcdef");
        copy_field(contact.public_key_hex, sizeof(contact.public_key_hex),
                   KEY_HEX);
        copy_field(contact.type, sizeof(contact.type), cases[i].type);
        contact.verification_source =
            D1L_CONTACT_VERIFICATION_URI_IMPORT;

        assert(d1l_contact_store_is_canonical(&contact));
        assert(d1l_contact_store_can_dm(&contact) == cases[i].can_dm);
        assert(d1l_contact_store_can_path_probe(&contact) ==
               cases[i].can_path_probe);
        assert(d1l_contact_store_can_admin(&contact) == cases[i].can_admin);
    }

    d1l_contact_entry_t unverified_repeater = {0};
    copy_field(unverified_repeater.fingerprint,
               sizeof(unverified_repeater.fingerprint),
               "0123456789abcdef");
    copy_field(unverified_repeater.public_key_hex,
               sizeof(unverified_repeater.public_key_hex), KEY_HEX);
    copy_field(unverified_repeater.type, sizeof(unverified_repeater.type),
               "repeater");
    assert(!d1l_contact_store_can_path_probe(&unverified_repeater));
}

static void test_contact_export_role_mapping(void)
{
    assert(d1l_contact_store_meshcore_type_id("chat") == 1U);
    assert(d1l_contact_store_meshcore_type_id("repeater") == 2U);
    assert(d1l_contact_store_meshcore_type_id("room") == 3U);
    assert(d1l_contact_store_meshcore_type_id("sensor") == 4U);
    assert(d1l_contact_store_meshcore_type_id("unknown") == 0U);
    assert(d1l_contact_store_meshcore_type_id("node") == 0U);
    assert(d1l_contact_store_meshcore_type_id(NULL) == 0U);

    static const char *types[] = {"chat", "repeater", "room", "sensor"};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        d1l_contact_entry_t contact = {0};
        copy_field(contact.public_key_hex, sizeof(contact.public_key_hex), KEY_HEX);
        copy_field(contact.fingerprint, sizeof(contact.fingerprint),
                   "0123456789abcdef");
        copy_field(contact.alias, sizeof(contact.alias), "North Room");
        copy_field(contact.type, sizeof(contact.type), types[i]);
        contact.verification_source = D1L_CONTACT_VERIFICATION_URI_IMPORT;
        char uri[D1L_CONTACT_EXPORT_URI_LEN] = {0};
        assert(d1l_contact_store_export_uri(&contact, uri, sizeof(uri)) == ESP_OK);
        assert(strstr(uri, "meshcore://contact/add?name=North+Room&public_key=") == uri);
        char expected_type[16] = {0};
        (void)snprintf(expected_type, sizeof(expected_type), "&type=%u",
                       (unsigned)(i + 1U));
        assert(strstr(uri, expected_type) != NULL);
    }

    d1l_contact_entry_t unknown = {0};
    copy_field(unknown.public_key_hex, sizeof(unknown.public_key_hex), KEY_HEX);
    copy_field(unknown.fingerprint, sizeof(unknown.fingerprint),
               "0123456789abcdef");
    copy_field(unknown.alias, sizeof(unknown.alias), "Unknown");
    copy_field(unknown.type, sizeof(unknown.type), "unknown");
    unknown.verification_source = D1L_CONTACT_VERIFICATION_URI_IMPORT;
    char uri[D1L_CONTACT_EXPORT_URI_LEN] = "not-empty";
    assert(d1l_contact_store_export_uri(&unknown, uri, sizeof(uri)) ==
           ESP_ERR_INVALID_STATE);
    assert(uri[0] == '\0');

    unknown.public_key_hex[0] = 'z';
    copy_field(unknown.type, sizeof(unknown.type), "chat");
    assert(d1l_contact_store_export_uri(&unknown, uri, sizeof(uri)) ==
           ESP_ERR_INVALID_STATE);
    assert(uri[0] == '\0');
}

int main(void)
{
    test_node_v3_to_v4_migration();
    test_contact_v3_to_v7_migration();
    test_contact_v4_to_v7_migration_is_truthful();
    test_contact_v5_to_v7_migration_preserves_provenance();
    test_contact_v6_to_v7_migration_creates_preboot_path_record();
    test_canonical_path_lifecycle_is_atomic_and_retained();
    test_forced_path_flush_rejects_prewrite_revision_change();
    test_forced_path_flush_rejects_during_write_revision_change();
    test_contact_persistence_revision_saturates_fail_closed();
    test_contact_next_sequence_exhaustion_preserves_ram_and_nvs();
    test_current_path_record_corruption_is_preserved_fail_closed();
    test_same_size_future_contact_schema_is_preserved_fail_closed();
    test_oversized_future_contact_schema_is_preserved_fail_closed();
    test_node_reachability_is_boot_local_and_wrap_safe();
    test_stale_advert_and_location_preservation();
    test_node_sd_failure_keeps_live_data_and_retry_succeeds();
    test_located_nodes_are_retained_until_explicit_clear();
    test_d1l_first_zero_timestamp_accepted();
    test_d1l_equal_timestamp_rejected_non_mutating();
    test_d1l_older_timestamp_rejected_non_mutating();
    test_d1l_strictly_newer_timestamp_accepted();
    test_d1l_uint32_max_then_zero_no_wrap();
    test_d1l_equal_exact_key_contact_retry();
    test_d1l_full_key_prefix_collision_precedes_replay();
    test_d1l_advert_accepted_terminal_hash_remembered();
    test_d1l_distinct_equal_timestamp_hash_miss_reaches_replay();
    test_d1l_contact_retry_failure_hash_not_remembered();
    test_d1l_accepted_contact_failure_hash_not_remembered();
    test_d1l_contact_retry_success_hash_remembered();
    test_contact_nvs_failure_rolls_back_and_retry_succeeds();
    test_prepare_after_concurrent_delete_fails_closed();
    test_verified_contact_create_reload_update_preserves_preferences();
    test_verified_contact_promotes_placeholder_without_losing_preferences();
    test_verified_contact_refuses_collision_and_rolls_back_nvs_failure();
    test_verified_contact_full_store_never_evicts();
    test_favorite_placeholders_are_never_evicted();
    test_uri_import_promotes_placeholder_and_preserves_preferences();
    test_uri_import_conflicts_and_nvs_failure_are_non_mutating();
    test_uri_import_preserves_official_names();
    test_full_key_chat_without_provenance_is_not_dm_capable();
    test_path_probe_roles_do_not_expand_dm_capability();
    test_contact_export_role_mapping();
    puts("native node/contact store behavior: ok");
    return 0;
}
