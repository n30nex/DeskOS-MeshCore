#include "channel_store.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "nvs.h"

#include "app/release_profile.h"
#include "mesh/store_lock.h"

#define D1L_CHANNEL_STORE_NAMESPACE "d1l_channels"
#define D1L_CHANNEL_STORE_KEY "channels"
#define D1L_CHANNEL_STORE_MAGIC UINT32_C(0x43484e4c)
#define D1L_CHANNEL_STORE_SCHEMA_V1 1U
#define D1L_CHANNEL_STORE_SCHEMA_V2 2U
#define D1L_CHANNEL_STORE_SCHEMA 3U
#define D1L_CHANNEL_URI_SCHEME "meshcore://channel/add?"
#define D1L_CHANNEL_PUBLIC_NAME "Public"

static const uint8_t s_public_secret[D1L_CHANNEL_SECRET_MAX_LEN] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static bool channel_available_in_release_profile(uint64_t channel_id)
{
    return channel_id == D1L_CHANNEL_PUBLIC_ID ||
        d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT);
}

/* Frozen v1 layout. It predates explicit source/import provenance and the
 * separate stable history key. */
typedef struct {
    uint64_t channel_id;
    uint32_t created_ms;
    uint32_t updated_ms;
    uint32_t newest_message_seq;
    uint32_t read_through_seq;
    uint32_t unread_count;
    char name[D1L_CHANNEL_NAME_LEN];
    uint8_t secret[D1L_CHANNEL_SECRET_MAX_LEN];
    uint8_t secret_len;
    uint8_t channel_hash;
    uint8_t enabled;
    uint8_t is_default;
    /* Explicit tail padding keeps the frozen blob layout independent of the
     * target ABI's uint64_t alignment. */
    uint8_t reserved[7];
} d1l_channel_entry_v1_t;

typedef struct {
    uint32_t schema;
    uint32_t count;
    uint64_t next_channel_id;
    d1l_channel_entry_v1_t entries[D1L_CHANNEL_STORE_CAPACITY];
} d1l_channel_store_blob_v1_t;

typedef struct {
    uint64_t channel_id;
    uint64_t history_key;
    uint32_t created_ms;
    uint32_t updated_ms;
    uint32_t imported_at_ms;
    uint32_t newest_message_seq;
    uint32_t read_through_seq;
    uint32_t unread_count;
    char name[D1L_CHANNEL_NAME_LEN];
    uint8_t secret[D1L_CHANNEL_SECRET_MAX_LEN];
    uint8_t secret_len;
    uint8_t channel_hash;
    uint8_t source;
    uint8_t enabled;
    uint8_t is_default;
    uint8_t reserved[2];
} d1l_channel_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t schema;
    uint32_t blob_length;
    uint32_t payload_length;
    uint32_t payload_checksum;
    uint32_t envelope_reserved;
    uint64_t lineage;
    uint64_t generation;
    uint64_t next_channel_id;
    uint32_t revision;
    uint32_t total_mutations;
    uint32_t count;
    uint32_t payload_reserved;
    d1l_channel_entry_t entries[D1L_CHANNEL_STORE_CAPACITY];
} d1l_channel_store_blob_v2_t;

typedef struct {
    uint32_t magic;
    uint32_t schema;
    uint32_t blob_length;
    uint32_t payload_length;
    uint32_t payload_checksum;
    uint32_t envelope_reserved;
    uint64_t lineage;
    uint64_t generation;
    uint64_t next_channel_id;
    uint32_t revision;
    uint32_t total_mutations;
    uint32_t count;
    uint32_t payload_reserved;
    uint32_t message_epoch;
    uint32_t message_next_seq;
    uint64_t message_clear_lineage;
    d1l_channel_entry_t entries[D1L_CHANNEL_STORE_CAPACITY];
} d1l_channel_store_blob_t;

_Static_assert(sizeof(d1l_channel_entry_v1_t) == 104U,
               "channel schema v1 entry layout changed");
_Static_assert(offsetof(d1l_channel_entry_v1_t, name) == 28U,
               "channel schema v1 name offset changed");
_Static_assert(offsetof(d1l_channel_entry_v1_t, secret) == 61U,
               "channel schema v1 secret offset changed");
_Static_assert(sizeof(d1l_channel_store_blob_v1_t) == 848U,
               "channel schema v1 blob layout changed");
_Static_assert(sizeof(d1l_channel_entry_t) == 112U,
               "channel schema v2 entry layout changed");
_Static_assert(offsetof(d1l_channel_entry_t, name) == 40U,
               "channel schema v2 name offset changed");
_Static_assert(offsetof(d1l_channel_entry_t, secret) == 73U,
               "channel schema v2 secret offset changed");
_Static_assert(offsetof(d1l_channel_store_blob_v2_t, lineage) == 24U,
               "channel schema v2 payload offset changed");
_Static_assert(offsetof(d1l_channel_store_blob_v2_t, entries) == 64U,
               "channel schema v2 entries offset changed");
_Static_assert(sizeof(d1l_channel_store_blob_v2_t) == 960U,
               "channel schema v2 blob layout changed");
_Static_assert(offsetof(d1l_channel_store_blob_t, lineage) == 24U,
               "channel schema v3 payload offset changed");
_Static_assert(offsetof(d1l_channel_store_blob_t, message_epoch) == 64U,
               "channel schema v3 message generation offset changed");
_Static_assert(offsetof(d1l_channel_store_blob_t, entries) == 80U,
               "channel schema v3 entries offset changed");
_Static_assert(sizeof(d1l_channel_store_blob_t) == 976U,
               "channel schema v3 blob layout changed");

static d1l_channel_entry_t s_entries[D1L_CHANNEL_STORE_CAPACITY];
static size_t s_count;
static uint64_t s_lineage;
static uint64_t s_generation;
static uint64_t s_next_channel_id;
static uint32_t s_revision;
static uint32_t s_total_mutations;
static uint32_t s_message_epoch;
static uint32_t s_message_next_seq;
static uint64_t s_message_clear_lineage;
static bool s_loaded;
static esp_err_t s_load_status = ESP_ERR_INVALID_STATE;
static d1l_channel_store_blob_t s_blob_scratch;
static d1l_channel_store_blob_t s_rollback_scratch;
static d1l_store_lock_t s_store_lock = D1L_STORE_LOCK_INITIALIZER;

typedef struct {
    char name[D1L_CHANNEL_NAME_LEN];
    uint8_t secret[D1L_CHANNEL_SECRET_MAX_LEN];
    uint8_t secret_len;
} d1l_channel_uri_t;

static void secure_zero(void *data, size_t len)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;
    while (bytes && len > 0U) {
        *bytes++ = 0U;
        len--;
    }
}

static uint64_t mix64(uint64_t value)
{
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static uint64_t random_nonzero_u64(uint64_t excluded)
{
    uint64_t value = 0U;
    while (value == 0U || value == D1L_CHANNEL_PUBLIC_ID ||
           value == excluded || value == UINT64_MAX) {
        value = ((uint64_t)esp_random() << 32U) | (uint64_t)esp_random();
    }
    return value;
}

static uint64_t initial_next_channel_id(uint64_t lineage)
{
    uint64_t candidate = mix64(lineage ^ UINT64_C(0xd1c4a66e1d5eed01));
    while (candidate <= D1L_CHANNEL_PUBLIC_ID || candidate == lineage ||
           candidate >= UINT64_MAX - 1U) {
        candidate = mix64(candidate + UINT64_C(0x9e3779b97f4a7c15));
    }
    return candidate;
}

static uint64_t history_key_for(uint64_t lineage, uint64_t channel_id)
{
    return lineage ^ channel_id;
}

static uint32_t crc32_bytes(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; bytes && i < len; ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

static uint32_t now_ms(void)
{
    return (uint32_t)((uint64_t)esp_timer_get_time() / UINT64_C(1000));
}

static bool source_is_valid(uint8_t source)
{
    return source >= D1L_CHANNEL_SOURCE_BUILTIN &&
           source <= D1L_CHANNEL_SOURCE_MIGRATED;
}

static bool byte_is_hex(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static uint8_t hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return (uint8_t)(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return (uint8_t)(10 + value - 'a');
    }
    return (uint8_t)(10 + value - 'A');
}

static bool utf8_name_is_valid(const unsigned char *text, size_t text_len)
{
    if (!text || text_len == 0U || text_len >= D1L_CHANNEL_NAME_LEN) {
        return false;
    }

    size_t cursor = 0U;
    while (cursor < text_len) {
        const unsigned char lead = text[cursor];
        uint32_t codepoint = 0U;
        uint32_t minimum = 0U;
        size_t continuation_count = 0U;

        if (lead <= 0x7fU) {
            codepoint = lead;
        } else if (lead >= 0xc2U && lead <= 0xdfU) {
            codepoint = (uint32_t)(lead & 0x1fU);
            minimum = 0x80U;
            continuation_count = 1U;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            codepoint = (uint32_t)(lead & 0x0fU);
            minimum = 0x800U;
            continuation_count = 2U;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            codepoint = (uint32_t)(lead & 0x07U);
            minimum = 0x10000U;
            continuation_count = 3U;
        } else {
            return false;
        }

        if (cursor + continuation_count >= text_len) {
            return false;
        }
        for (size_t i = 1U; i <= continuation_count; ++i) {
            const unsigned char continuation = text[cursor + i];
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) |
                        (uint32_t)(continuation & 0x3fU);
        }

        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            codepoint <= 0x1fU ||
            (codepoint >= 0x7fU && codepoint <= 0x9fU)) {
            return false;
        }
        cursor += continuation_count + 1U;
    }
    return true;
}

static bool bounded_name_is_valid(const char name[D1L_CHANNEL_NAME_LEN])
{
    if (!name) {
        return false;
    }
    for (size_t len = 0U; len < D1L_CHANNEL_NAME_LEN; ++len) {
        if (name[len] == '\0') {
            return utf8_name_is_valid((const unsigned char *)name, len);
        }
    }
    return false;
}

static bool input_name_is_valid(const char *name)
{
    if (!name) {
        return false;
    }
    size_t len = 0U;
    while (len < D1L_CHANNEL_NAME_LEN && name[len] != '\0') {
        len++;
    }
    return len < D1L_CHANNEL_NAME_LEN &&
           utf8_name_is_valid((const unsigned char *)name, len);
}

static unsigned char ascii_fold(unsigned char value)
{
    return value >= 'A' && value <= 'Z'
               ? (unsigned char)(value + ('a' - 'A'))
               : value;
}

static bool names_equal(const char *left, const char *right)
{
    if (!left || !right) {
        return false;
    }
    size_t i = 0U;
    while (i < D1L_CHANNEL_NAME_LEN) {
        const unsigned char a = (unsigned char)left[i];
        const unsigned char b = (unsigned char)right[i];
        if (ascii_fold(a) != ascii_fold(b)) {
            return false;
        }
        if (a == '\0' || b == '\0') {
            return a == b;
        }
        i++;
    }
    return false;
}

static bool tail_is_zero(const uint8_t *secret)
{
    if (!secret) {
        return false;
    }
    for (size_t i = D1L_CHANNEL_SECRET_128_LEN;
         i < D1L_CHANNEL_SECRET_MAX_LEN; ++i) {
        if (secret[i] != 0U) {
            return false;
        }
    }
    return true;
}

static bool normalize_secret(const uint8_t *secret, uint8_t secret_len,
                             uint8_t dest[D1L_CHANNEL_SECRET_MAX_LEN],
                             uint8_t *out_len)
{
    if (!secret || !dest || !out_len ||
        (secret_len != D1L_CHANNEL_SECRET_128_LEN &&
         secret_len != D1L_CHANNEL_SECRET_256_LEN)) {
        return false;
    }
    memset(dest, 0, D1L_CHANNEL_SECRET_MAX_LEN);
    memcpy(dest, secret, secret_len);
    *out_len = secret_len;
    if (secret_len == D1L_CHANNEL_SECRET_256_LEN && tail_is_zero(dest)) {
        *out_len = D1L_CHANNEL_SECRET_128_LEN;
    }
    return true;
}

static bool stored_secret_is_valid(const d1l_channel_entry_t *entry)
{
    if (!entry ||
        (entry->secret_len != D1L_CHANNEL_SECRET_128_LEN &&
         entry->secret_len != D1L_CHANNEL_SECRET_256_LEN)) {
        return false;
    }
    if (entry->secret_len == D1L_CHANNEL_SECRET_128_LEN) {
        return tail_is_zero(entry->secret);
    }
    return !tail_is_zero(entry->secret);
}

static bool stored_v1_secret_is_valid(const d1l_channel_entry_v1_t *entry)
{
    if (!entry ||
        (entry->secret_len != D1L_CHANNEL_SECRET_128_LEN &&
         entry->secret_len != D1L_CHANNEL_SECRET_256_LEN)) {
        return false;
    }
    if (entry->secret_len == D1L_CHANNEL_SECRET_128_LEN) {
        return tail_is_zero(entry->secret);
    }
    return !tail_is_zero(entry->secret);
}

static bool secrets_equal(const uint8_t *left, uint8_t left_len,
                          const uint8_t *right, uint8_t right_len)
{
    return left && right && left_len == right_len &&
           memcmp(left, right, left_len) == 0;
}

esp_err_t d1l_channel_store_protocol_hash(const uint8_t *secret,
                                          uint8_t secret_len,
                                          uint8_t *out_hash)
{
    if (!secret || !out_hash ||
        (secret_len != D1L_CHANNEL_SECRET_128_LEN &&
         secret_len != D1L_CHANNEL_SECRET_256_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t effective_len =
        secret_len == D1L_CHANNEL_SECRET_256_LEN && tail_is_zero(secret)
            ? D1L_CHANNEL_SECRET_128_LEN
            : secret_len;
    uint8_t digest[32] = {0};
    const mbedtls_md_info_t *md =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md(md, secret, effective_len, digest) != 0) {
        secure_zero(digest, sizeof(digest));
        return ESP_FAIL;
    }
    *out_hash = digest[0];
    secure_zero(digest, sizeof(digest));
    return ESP_OK;
}

static void copy_info(const d1l_channel_entry_t *entry,
                      d1l_channel_info_t *out_info)
{
    if (!out_info) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (!entry) {
        return;
    }
    out_info->channel_id = entry->channel_id;
    out_info->history_key = entry->history_key;
    out_info->created_ms = entry->created_ms;
    out_info->updated_ms = entry->updated_ms;
    out_info->imported_at_ms = entry->imported_at_ms;
    out_info->newest_message_seq = entry->newest_message_seq;
    out_info->read_through_seq = entry->read_through_seq;
    out_info->unread_count = entry->unread_count;
    memcpy(out_info->name, entry->name, sizeof(out_info->name));
    out_info->channel_hash = entry->channel_hash;
    out_info->source = entry->source;
    out_info->enabled = entry->enabled != 0U;
    out_info->is_default = entry->is_default != 0U;
}

static void copy_protocol_key(const d1l_channel_entry_t *entry,
                              d1l_channel_protocol_key_t *out_key)
{
    memset(out_key, 0, sizeof(*out_key));
    out_key->channel_id = entry->channel_id;
    out_key->channel_hash = entry->channel_hash;
    out_key->secret_len = entry->secret_len;
    memcpy(out_key->secret, entry->secret, sizeof(out_key->secret));
}

static int index_by_id(uint64_t channel_id)
{
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[i].channel_id == channel_id) {
            return (int)i;
        }
    }
    return -1;
}

static int index_by_name(const char *name, int excluded_index)
{
    for (size_t i = 0U; i < s_count; ++i) {
        if ((int)i != excluded_index && names_equal(s_entries[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static int index_by_secret(const uint8_t *secret, uint8_t secret_len)
{
    for (size_t i = 0U; i < s_count; ++i) {
        if (secrets_equal(s_entries[i].secret, s_entries[i].secret_len,
                          secret, secret_len)) {
            return (int)i;
        }
    }
    return -1;
}

static int default_index(void)
{
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[i].is_default != 0U) {
            return (int)i;
        }
    }
    return -1;
}

static void make_only_default(size_t index)
{
    for (size_t i = 0U; i < s_count; ++i) {
        s_entries[i].is_default = i == index ? 1U : 0U;
    }
}

static bool mutation_counter_available(void)
{
    return s_generation != UINT64_MAX && s_revision != UINT32_MAX &&
           s_total_mutations != UINT32_MAX;
}

static void note_mutation(void)
{
    s_generation++;
    s_revision++;
    s_total_mutations++;
}

static esp_err_t seed_public_ram(uint64_t lineage, uint64_t generation,
                                uint64_t next_channel_id)
{
    secure_zero(s_entries, sizeof(s_entries));
    s_count = 1U;
    s_lineage = lineage;
    s_generation = generation;
    s_next_channel_id = next_channel_id;
    s_revision = 1U;
    s_total_mutations = 0U;
    s_message_epoch = 0U;
    s_message_next_seq = 0U;
    s_message_clear_lineage = 0U;

    d1l_channel_entry_t *entry = &s_entries[0];
    entry->channel_id = D1L_CHANNEL_PUBLIC_ID;
    entry->history_key = history_key_for(s_lineage, entry->channel_id);
    (void)snprintf(entry->name, sizeof(entry->name), "%s",
                   D1L_CHANNEL_PUBLIC_NAME);
    memcpy(entry->secret, s_public_secret, sizeof(entry->secret));
    entry->secret_len = D1L_CHANNEL_SECRET_128_LEN;
    entry->source = D1L_CHANNEL_SOURCE_BUILTIN;
    entry->enabled = 1U;
    entry->is_default = 1U;
    return d1l_channel_store_protocol_hash(entry->secret, entry->secret_len,
                                           &entry->channel_hash);
}

static void clear_unloaded_ram(void)
{
    secure_zero(s_entries, sizeof(s_entries));
    s_count = 0U;
    s_lineage = 0U;
    s_generation = 0U;
    s_next_channel_id = 0U;
    s_revision = 0U;
    s_total_mutations = 0U;
    s_message_epoch = 0U;
    s_message_next_seq = 0U;
    s_message_clear_lineage = 0U;
    s_loaded = false;
}

static void fill_blob(d1l_channel_store_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->magic = D1L_CHANNEL_STORE_MAGIC;
    blob->schema = D1L_CHANNEL_STORE_SCHEMA;
    blob->blob_length = (uint32_t)sizeof(*blob);
    blob->payload_length =
        (uint32_t)(sizeof(*blob) - offsetof(d1l_channel_store_blob_t, lineage));
    blob->lineage = s_lineage;
    blob->generation = s_generation;
    blob->next_channel_id = s_next_channel_id;
    blob->revision = s_revision;
    blob->total_mutations = s_total_mutations;
    blob->count = (uint32_t)s_count;
    blob->message_epoch = s_message_epoch;
    blob->message_next_seq = s_message_next_seq;
    blob->message_clear_lineage = s_message_clear_lineage;
    memcpy(blob->entries, s_entries, sizeof(s_entries));
    blob->payload_checksum = crc32_bytes(
        &blob->lineage, (size_t)blob->payload_length);
}

static void restore_blob(const d1l_channel_store_blob_t *blob)
{
    if (!blob) {
        return;
    }
    secure_zero(s_entries, sizeof(s_entries));
    s_count = blob->count <= D1L_CHANNEL_STORE_CAPACITY ? blob->count : 0U;
    s_lineage = blob->lineage;
    s_generation = blob->generation;
    s_next_channel_id = blob->next_channel_id;
    s_revision = blob->revision;
    s_total_mutations = blob->total_mutations;
    s_message_epoch = blob->message_epoch;
    s_message_next_seq = blob->message_next_seq;
    s_message_clear_lineage = blob->message_clear_lineage;
    memcpy(s_entries, blob->entries, s_count * sizeof(s_entries[0]));
}

static esp_err_t persist_store(void)
{
    secure_zero(&s_blob_scratch, sizeof(s_blob_scratch));
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_CHANNEL_STORE_NAMESPACE, NVS_READWRITE,
                             &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    fill_blob(&s_blob_scratch);
    ret = nvs_set_blob(handle, D1L_CHANNEL_STORE_KEY, &s_blob_scratch,
                       sizeof(s_blob_scratch));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    secure_zero(&s_blob_scratch, sizeof(s_blob_scratch));
    return ret;
}

static esp_err_t persist_store_or_rollback(
    d1l_channel_store_blob_t *before)
{
    const esp_err_t ret = persist_store();
    if (ret != ESP_OK) {
        restore_blob(before);
    }
    secure_zero(before, sizeof(*before));
    return ret;
}

static bool public_entry_is_exact(const d1l_channel_entry_t *entry,
                                  uint64_t lineage)
{
    return entry && entry->channel_id == D1L_CHANNEL_PUBLIC_ID &&
           entry->history_key ==
               history_key_for(lineage, D1L_CHANNEL_PUBLIC_ID) &&
           strcmp(entry->name, D1L_CHANNEL_PUBLIC_NAME) == 0 &&
           entry->secret_len == D1L_CHANNEL_SECRET_128_LEN &&
           memcmp(entry->secret, s_public_secret,
                  sizeof(entry->secret)) == 0 &&
           entry->source == D1L_CHANNEL_SOURCE_BUILTIN &&
           entry->enabled == 1U;
}

static bool entry_is_valid(const d1l_channel_entry_t *entry, uint64_t lineage)
{
    if (!entry || entry->channel_id == 0U ||
        entry->channel_id == lineage ||
        entry->history_key != history_key_for(lineage, entry->channel_id) ||
        !bounded_name_is_valid(entry->name) ||
        !stored_secret_is_valid(entry) ||
        !source_is_valid(entry->source) || entry->enabled > 1U ||
        entry->is_default > 1U ||
        (entry->is_default != 0U && entry->enabled == 0U) ||
        entry->read_through_seq > entry->newest_message_seq ||
        entry->unread_count > entry->newest_message_seq ||
        (entry->unread_count == 0U &&
         entry->read_through_seq != entry->newest_message_seq) ||
        (entry->unread_count != 0U &&
         entry->read_through_seq == entry->newest_message_seq)) {
        return false;
    }
    uint8_t expected_hash = 0U;
    return d1l_channel_store_protocol_hash(entry->secret, entry->secret_len,
                                           &expected_hash) == ESP_OK &&
           expected_hash == entry->channel_hash;
}

static bool stored_entries_are_valid(const d1l_channel_entry_t *entries,
                                     size_t count, uint64_t lineage,
                                     uint64_t next_channel_id)
{
    size_t public_count = 0U;
    size_t default_count = 0U;
    uint64_t max_id = 0U;
    for (size_t i = 0U; i < count; ++i) {
        const d1l_channel_entry_t *entry = &entries[i];
        if (!entry_is_valid(entry, lineage)) {
            return false;
        }
        if (entry->channel_id == D1L_CHANNEL_PUBLIC_ID) {
            if (!public_entry_is_exact(entry, lineage)) {
                return false;
            }
            public_count++;
        } else if (entry->source == D1L_CHANNEL_SOURCE_BUILTIN) {
            return false;
        }
        if (entry->is_default != 0U) {
            default_count++;
        }
        if (entry->channel_id > max_id) {
            max_id = entry->channel_id;
        }
        for (size_t j = 0U; j < i; ++j) {
            const d1l_channel_entry_t *other = &entries[j];
            if (other->channel_id == entry->channel_id ||
                other->history_key == entry->history_key ||
                names_equal(other->name, entry->name) ||
                secrets_equal(other->secret, other->secret_len, entry->secret,
                              entry->secret_len)) {
                return false;
            }
        }
    }
    return public_count == 1U && default_count == 1U &&
           next_channel_id > max_id;
}

static bool blob_is_valid(const d1l_channel_store_blob_t *blob, size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->magic != D1L_CHANNEL_STORE_MAGIC ||
        blob->schema != D1L_CHANNEL_STORE_SCHEMA ||
        blob->blob_length != (uint32_t)sizeof(*blob) ||
        blob->payload_length !=
            (uint32_t)(sizeof(*blob) -
                       offsetof(d1l_channel_store_blob_t, lineage)) ||
        blob->envelope_reserved != 0U || blob->payload_reserved != 0U ||
        blob->payload_checksum !=
            crc32_bytes(&blob->lineage, (size_t)blob->payload_length) ||
        blob->lineage == 0U || blob->lineage == D1L_CHANNEL_PUBLIC_ID ||
        blob->generation == 0U || blob->count == 0U ||
        blob->count > D1L_CHANNEL_STORE_CAPACITY ||
        blob->next_channel_id <= D1L_CHANNEL_PUBLIC_ID ||
        blob->next_channel_id == UINT64_MAX ||
        blob->revision == 0U ||
        (blob->message_epoch == 0U &&
         (blob->message_next_seq != 0U ||
          blob->message_clear_lineage != 0U)) ||
        (blob->message_epoch != 0U && blob->message_next_seq == 0U)) {
        return false;
    }
    return stored_entries_are_valid(
        blob->entries, blob->count, blob->lineage, blob->next_channel_id);
}

static bool blob_v2_is_valid(const d1l_channel_store_blob_v2_t *blob,
                             size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->magic != D1L_CHANNEL_STORE_MAGIC ||
        blob->schema != D1L_CHANNEL_STORE_SCHEMA_V2 ||
        blob->blob_length != (uint32_t)sizeof(*blob) ||
        blob->payload_length !=
            (uint32_t)(sizeof(*blob) -
                       offsetof(d1l_channel_store_blob_v2_t, lineage)) ||
        blob->envelope_reserved != 0U || blob->payload_reserved != 0U ||
        blob->payload_checksum !=
            crc32_bytes(&blob->lineage, (size_t)blob->payload_length) ||
        blob->lineage == 0U || blob->lineage == D1L_CHANNEL_PUBLIC_ID ||
        blob->generation == 0U || blob->count == 0U ||
        blob->count > D1L_CHANNEL_STORE_CAPACITY ||
        blob->next_channel_id <= D1L_CHANNEL_PUBLIC_ID ||
        blob->next_channel_id == UINT64_MAX || blob->revision == 0U) {
        return false;
    }
    return stored_entries_are_valid(
        blob->entries, blob->count, blob->lineage, blob->next_channel_id);
}

static bool v1_entry_is_valid(const d1l_channel_entry_v1_t *entry)
{
    if (!entry || entry->channel_id == 0U ||
        !bounded_name_is_valid(entry->name) ||
        !stored_v1_secret_is_valid(entry) || entry->enabled > 1U ||
        entry->is_default > 1U ||
        (entry->is_default != 0U && entry->enabled == 0U) ||
        entry->read_through_seq > entry->newest_message_seq ||
        entry->unread_count > entry->newest_message_seq ||
        (entry->unread_count == 0U &&
         entry->read_through_seq != entry->newest_message_seq) ||
        (entry->unread_count != 0U &&
         entry->read_through_seq == entry->newest_message_seq)) {
        return false;
    }
    uint8_t expected_hash = 0U;
    return d1l_channel_store_protocol_hash(entry->secret, entry->secret_len,
                                           &expected_hash) == ESP_OK &&
           expected_hash == entry->channel_hash;
}

static bool blob_v1_is_valid(const d1l_channel_store_blob_v1_t *blob,
                             size_t len)
{
    if (!blob || len != sizeof(*blob) ||
        blob->schema != D1L_CHANNEL_STORE_SCHEMA_V1 || blob->count == 0U ||
        blob->count > D1L_CHANNEL_STORE_CAPACITY ||
        blob->next_channel_id <= D1L_CHANNEL_PUBLIC_ID ||
        blob->next_channel_id == UINT64_MAX) {
        return false;
    }
    size_t public_count = 0U;
    size_t default_count = 0U;
    uint64_t max_id = 0U;
    for (size_t i = 0U; i < blob->count; ++i) {
        const d1l_channel_entry_v1_t *entry = &blob->entries[i];
        if (!v1_entry_is_valid(entry)) {
            return false;
        }
        if (entry->channel_id == D1L_CHANNEL_PUBLIC_ID) {
            if (strcmp(entry->name, D1L_CHANNEL_PUBLIC_NAME) != 0 ||
                entry->secret_len != D1L_CHANNEL_SECRET_128_LEN ||
                memcmp(entry->secret, s_public_secret,
                       sizeof(entry->secret)) != 0 ||
                entry->enabled != 1U) {
                return false;
            }
            public_count++;
        }
        if (entry->is_default != 0U) {
            default_count++;
        }
        if (entry->channel_id > max_id) {
            max_id = entry->channel_id;
        }
        for (size_t j = 0U; j < i; ++j) {
            const d1l_channel_entry_v1_t *other = &blob->entries[j];
            if (other->channel_id == entry->channel_id ||
                names_equal(other->name, entry->name) ||
                secrets_equal(other->secret, other->secret_len, entry->secret,
                              entry->secret_len)) {
                return false;
            }
        }
    }
    return public_count == 1U && default_count == 1U &&
           blob->next_channel_id > max_id;
}

static void migrate_v2_blob(const d1l_channel_store_blob_v2_t *old_blob)
{
    secure_zero(s_entries, sizeof(s_entries));
    s_count = old_blob->count;
    s_lineage = old_blob->lineage;
    s_generation = old_blob->generation;
    s_next_channel_id = old_blob->next_channel_id;
    s_revision = old_blob->revision;
    s_total_mutations = old_blob->total_mutations;
    s_message_epoch = 0U;
    s_message_next_seq = 0U;
    s_message_clear_lineage = 0U;
    memcpy(s_entries, old_blob->entries,
           s_count * sizeof(s_entries[0]));
}

static void migrate_v1_blob(const d1l_channel_store_blob_v1_t *old_blob)
{
    uint64_t lineage = 0U;
    bool lineage_conflict = true;
    while (lineage_conflict) {
        lineage = random_nonzero_u64(lineage);
        lineage_conflict = false;
        for (size_t i = 0U; i < old_blob->count; ++i) {
            if (old_blob->entries[i].channel_id == lineage) {
                lineage_conflict = true;
                break;
            }
        }
    }
    secure_zero(s_entries, sizeof(s_entries));
    s_count = old_blob->count;
    s_lineage = lineage;
    s_generation = 1U;
    s_next_channel_id = old_blob->next_channel_id;
    s_revision = 1U;
    s_total_mutations = 0U;
    s_message_epoch = 0U;
    s_message_next_seq = 0U;
    s_message_clear_lineage = 0U;
    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_channel_entry_v1_t *source = &old_blob->entries[i];
        d1l_channel_entry_t *dest = &s_entries[i];
        dest->channel_id = source->channel_id;
        dest->history_key = history_key_for(s_lineage, source->channel_id);
        dest->created_ms = source->created_ms;
        dest->updated_ms = source->updated_ms;
        dest->newest_message_seq = source->newest_message_seq;
        dest->read_through_seq = source->read_through_seq;
        dest->unread_count = source->unread_count;
        memcpy(dest->name, source->name, sizeof(dest->name));
        memcpy(dest->secret, source->secret, sizeof(dest->secret));
        dest->secret_len = source->secret_len;
        dest->channel_hash = source->channel_hash;
        dest->source = source->channel_id == D1L_CHANNEL_PUBLIC_ID
                           ? D1L_CHANNEL_SOURCE_BUILTIN
                           : D1L_CHANNEL_SOURCE_MIGRATED;
        dest->enabled = source->enabled;
        dest->is_default = source->is_default;
    }
}

static esp_err_t ensure_loaded(void)
{
    d1l_store_lock_take(&s_store_lock);
    const bool loaded = s_loaded;
    d1l_store_lock_give(&s_store_lock);
    return loaded ? ESP_OK : d1l_channel_store_init();
}

esp_err_t d1l_channel_store_init(void)
{
    d1l_store_lock_take(&s_store_lock);
    const uint64_t seed_lineage = random_nonzero_u64(s_lineage);
    esp_err_t ret = seed_public_ram(
        seed_lineage, 1U, initial_next_channel_id(seed_lineage));
    s_loaded = false;
    s_load_status = ret;
    if (ret != ESP_OK) {
        d1l_store_lock_give(&s_store_lock);
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(D1L_CHANNEL_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        s_load_status = ret;
        d1l_store_lock_give(&s_store_lock);
        return ret;
    }

    secure_zero(&s_blob_scratch, sizeof(s_blob_scratch));
    size_t len = sizeof(s_blob_scratch);
    ret = nvs_get_blob(handle, D1L_CHANNEL_STORE_KEY, &s_blob_scratch, &len);
    bool must_persist = false;
    bool migrated_legacy = false;
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
        must_persist = true;
    } else if (ret == ESP_OK && blob_is_valid(&s_blob_scratch, len)) {
        restore_blob(&s_blob_scratch);
    } else if (ret == ESP_OK &&
               blob_v2_is_valid(
                   (const d1l_channel_store_blob_v2_t *)&s_blob_scratch,
                   len)) {
        migrate_v2_blob(
            (const d1l_channel_store_blob_v2_t *)&s_blob_scratch);
        must_persist = true;
        migrated_legacy = true;
    } else if (ret == ESP_OK &&
               blob_v1_is_valid(
                   (const d1l_channel_store_blob_v1_t *)&s_blob_scratch,
                   len)) {
        migrate_v1_blob(
            (const d1l_channel_store_blob_v1_t *)&s_blob_scratch);
        must_persist = true;
        migrated_legacy = true;
    } else if (ret == ESP_OK) {
        /* Preserve the corrupt/unknown blob for recovery. No channel secret
         * from an unproven layout is ever admitted into live state. */
        ret = ESP_ERR_INVALID_STATE;
    }
    nvs_close(handle);
    secure_zero(&s_blob_scratch, sizeof(s_blob_scratch));

    if (ret == ESP_OK && must_persist) {
        ret = persist_store();
    }
    if (ret != ESP_OK && migrated_legacy) {
        clear_unloaded_ram();
    }
    s_loaded = ret == ESP_OK;
    s_load_status = ret;
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

esp_err_t d1l_channel_store_reset(void)
{
    d1l_store_lock_take(&s_store_lock);
    const bool was_loaded = s_loaded;
    const esp_err_t previous_load_status = s_load_status;
    const uint64_t previous_lineage = s_lineage;
    const uint64_t previous_generation = s_generation;
    const uint64_t previous_next_channel_id = s_next_channel_id;
    if (was_loaded) {
        fill_blob(&s_rollback_scratch);
    } else {
        secure_zero(&s_rollback_scratch, sizeof(s_rollback_scratch));
    }
    const uint64_t reset_lineage = random_nonzero_u64(previous_lineage);
    uint64_t reset_generation = 1U;
    if (was_loaded) {
        reset_generation = previous_generation == UINT64_MAX
                               ? random_nonzero_u64(previous_generation)
                               : previous_generation + 1U;
    }
    const uint64_t reset_next_channel_id =
        was_loaded ? previous_next_channel_id
                   : initial_next_channel_id(reset_lineage);
    esp_err_t ret = seed_public_ram(reset_lineage, reset_generation,
                                    reset_next_channel_id);
    if (ret == ESP_OK) {
        ret = persist_store();
    }
    if (ret != ESP_OK && was_loaded) {
        restore_blob(&s_rollback_scratch);
    }
    secure_zero(&s_rollback_scratch, sizeof(s_rollback_scratch));
    s_loaded = ret == ESP_OK || was_loaded;
    s_load_status = ret == ESP_OK
                        ? ESP_OK
                        : (was_loaded ? previous_load_status : ret);
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

static esp_err_t create_locked(
    const char *name, const uint8_t *secret, uint8_t secret_len,
    uint8_t channel_hash, uint8_t source, bool enabled, bool make_default,
    uint32_t imported_at_ms, d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_info)
{
    const int secret_index = index_by_secret(secret, secret_len);
    if (secret_index >= 0) {
        if (names_equal(s_entries[secret_index].name, name)) {
            *out_result = D1L_CHANNEL_MUTATION_EXISTS;
            copy_info(&s_entries[secret_index], out_info);
            return ESP_OK;
        }
        *out_result = D1L_CHANNEL_MUTATION_SECRET_COLLISION;
        return ESP_ERR_INVALID_STATE;
    }
    if (index_by_name(name, -1) >= 0) {
        *out_result = D1L_CHANNEL_MUTATION_NAME_COLLISION;
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t candidate_id = s_next_channel_id;
    if (candidate_id == s_lineage && candidate_id < UINT64_MAX) {
        candidate_id++;
    }
    if (s_count >= D1L_CHANNEL_STORE_CAPACITY ||
        candidate_id <= D1L_CHANNEL_PUBLIC_ID ||
        candidate_id == s_lineage || candidate_id >= UINT64_MAX - 1U) {
        *out_result = D1L_CHANNEL_MUTATION_FULL;
        return ESP_ERR_NO_MEM;
    }
    if (!mutation_counter_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    fill_blob(&s_rollback_scratch);
    const size_t index = s_count++;
    d1l_channel_entry_t *entry = &s_entries[index];
    memset(entry, 0, sizeof(*entry));
    entry->channel_id = candidate_id;
    entry->history_key = history_key_for(s_lineage, entry->channel_id);
    s_next_channel_id = candidate_id + 1U;
    entry->created_ms = now_ms();
    entry->updated_ms = entry->created_ms;
    entry->imported_at_ms = imported_at_ms;
    memcpy(entry->name, name, strlen(name) + 1U);
    memcpy(entry->secret, secret, secret_len);
    entry->secret_len = secret_len;
    entry->channel_hash = channel_hash;
    entry->source = source;
    entry->enabled = enabled || make_default ? 1U : 0U;
    entry->is_default = 0U;
    if (make_default) {
        make_only_default(index);
    }
    note_mutation();
    const esp_err_t ret = persist_store_or_rollback(&s_rollback_scratch);
    if (ret == ESP_OK) {
        *out_result = D1L_CHANNEL_MUTATION_CREATED;
        copy_info(&s_entries[index], out_info);
    }
    return ret;
}

esp_err_t d1l_channel_store_add(
    const char *name, const uint8_t *secret, uint8_t secret_len,
    bool enabled, bool make_default, d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_info)
{
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = D1L_CHANNEL_MUTATION_NONE;
    if (out_info) {
        memset(out_info, 0, sizeof(*out_info));
    }
    uint8_t normalized[D1L_CHANNEL_SECRET_MAX_LEN] = {0};
    uint8_t normalized_len = 0U;
    if (!input_name_is_valid(name) ||
        !normalize_secret(secret, secret_len, normalized, &normalized_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t channel_hash = 0U;
    esp_err_t ret = d1l_channel_store_protocol_hash(
        normalized, normalized_len, &channel_hash);
    if (ret != ESP_OK) {
        secure_zero(normalized, sizeof(normalized));
        return ret;
    }
    ret = ensure_loaded();
    if (ret != ESP_OK) {
        secure_zero(normalized, sizeof(normalized));
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    ret = create_locked(name, normalized, normalized_len, channel_hash,
                        D1L_CHANNEL_SOURCE_MANUAL, enabled, make_default, 0U,
                        out_result, out_info);
    d1l_store_lock_give(&s_store_lock);
    secure_zero(normalized, sizeof(normalized));
    return ret;
}

static bool query_field_is(const char *field, size_t field_len,
                           const char *expected)
{
    const size_t expected_len = strlen(expected);
    return field_len == expected_len &&
           memcmp(field, expected, expected_len) == 0;
}

static bool decode_uri_name(const char *encoded, size_t encoded_len,
                            char dest[D1L_CHANNEL_NAME_LEN])
{
    size_t out = 0U;
    for (size_t i = 0U; i < encoded_len; ++i) {
        unsigned char value = (unsigned char)encoded[i];
        if (value == '%') {
            if (i + 2U >= encoded_len || !byte_is_hex(encoded[i + 1U]) ||
                !byte_is_hex(encoded[i + 2U])) {
                return false;
            }
            value = (unsigned char)((hex_value(encoded[i + 1U]) << 4U) |
                                    hex_value(encoded[i + 2U]));
            i += 2U;
        } else if (value == '+') {
            value = ' ';
        }
        if (value == 0U || out + 1U >= D1L_CHANNEL_NAME_LEN) {
            return false;
        }
        dest[out++] = (char)value;
    }
    dest[out] = '\0';
    return utf8_name_is_valid((const unsigned char *)dest, out);
}

static bool decode_uri_secret(const char *encoded, size_t encoded_len,
                              uint8_t dest[D1L_CHANNEL_SECRET_MAX_LEN],
                              uint8_t *out_len)
{
    if (!dest || !out_len ||
        encoded_len != D1L_CHANNEL_SECRET_128_LEN * 2U) {
        return false;
    }
    const size_t decoded_len = encoded_len / 2U;
    memset(dest, 0, D1L_CHANNEL_SECRET_MAX_LEN);
    for (size_t i = 0U; i < decoded_len; ++i) {
        if (!byte_is_hex(encoded[i * 2U]) ||
            !byte_is_hex(encoded[i * 2U + 1U])) {
            secure_zero(dest, D1L_CHANNEL_SECRET_MAX_LEN);
            return false;
        }
        dest[i] = (uint8_t)((hex_value(encoded[i * 2U]) << 4U) |
                            hex_value(encoded[i * 2U + 1U]));
    }
    *out_len = D1L_CHANNEL_SECRET_128_LEN;
    return true;
}

static bool parse_channel_uri(const char *uri, size_t uri_len,
                              d1l_channel_uri_t *out)
{
    if (!uri || !out || uri_len == 0U ||
        uri_len >= D1L_CHANNEL_SHARE_URI_LEN) {
        return false;
    }
    for (size_t i = 0U; i < uri_len; ++i) {
        if (uri[i] == '\0') {
            return false;
        }
    }
    const size_t prefix_len = sizeof(D1L_CHANNEL_URI_SCHEME) - 1U;
    if (uri_len <= prefix_len ||
        memcmp(uri, D1L_CHANNEL_URI_SCHEME, prefix_len) != 0) {
        return false;
    }

    d1l_channel_uri_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    bool parsed_ok = false;
    bool have_name = false;
    bool have_secret = false;
    size_t cursor = prefix_len;
    while (cursor < uri_len) {
        size_t field_end = cursor;
        while (field_end < uri_len && uri[field_end] != '&') {
            field_end++;
        }
        if (field_end == cursor) {
            goto cleanup;
        }
        size_t equals = cursor;
        while (equals < field_end && uri[equals] != '=') {
            equals++;
        }
        if (equals == cursor || equals == field_end) {
            goto cleanup;
        }
        for (size_t i = equals + 1U; i < field_end; ++i) {
            if (uri[i] == '=') {
                goto cleanup;
            }
        }
        const char *field = &uri[cursor];
        const size_t field_len = equals - cursor;
        const char *value = &uri[equals + 1U];
        const size_t value_len = field_end - equals - 1U;
        if (query_field_is(field, field_len, "name")) {
            if (have_name ||
                !decode_uri_name(value, value_len, parsed.name)) {
                goto cleanup;
            }
            have_name = true;
        } else if (query_field_is(field, field_len, "secret")) {
            if (have_secret || !decode_uri_secret(
                                   value, value_len, parsed.secret,
                                   &parsed.secret_len)) {
                goto cleanup;
            }
            have_secret = true;
        } else {
            goto cleanup;
        }
        cursor = field_end == uri_len ? uri_len : field_end + 1U;
        if (cursor == uri_len && field_end != uri_len) {
            goto cleanup;
        }
    }
    if (!have_name || !have_secret) {
        goto cleanup;
    }
    *out = parsed;
    parsed_ok = true;

cleanup:
    secure_zero(parsed.secret, sizeof(parsed.secret));
    return parsed_ok;
}

esp_err_t d1l_channel_store_import_uri(
    const char *uri, size_t uri_len,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_info)
{
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = D1L_CHANNEL_MUTATION_NONE;
    if (out_info) {
        memset(out_info, 0, sizeof(*out_info));
    }
    d1l_channel_uri_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    if (!parse_channel_uri(uri, uri_len, &parsed)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t channel_hash = 0U;
    esp_err_t ret = d1l_channel_store_protocol_hash(
        parsed.secret, parsed.secret_len, &channel_hash);
    if (ret != ESP_OK) {
        secure_zero(parsed.secret, sizeof(parsed.secret));
        return ret;
    }
    ret = ensure_loaded();
    if (ret != ESP_OK) {
        secure_zero(parsed.secret, sizeof(parsed.secret));
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    const int secret_index = index_by_secret(parsed.secret, parsed.secret_len);
    if (secret_index < 0) {
        ret = create_locked(parsed.name, parsed.secret, parsed.secret_len,
                            channel_hash, D1L_CHANNEL_SOURCE_URI_IMPORT, true,
                            false, now_ms(), out_result, out_info);
    } else {
        d1l_channel_entry_t *entry = &s_entries[secret_index];
        if (entry->channel_id == D1L_CHANNEL_PUBLIC_ID &&
            strcmp(parsed.name, D1L_CHANNEL_PUBLIC_NAME) != 0) {
            *out_result = D1L_CHANNEL_MUTATION_PROTECTED;
            ret = ESP_ERR_NOT_SUPPORTED;
        } else if (index_by_name(parsed.name, secret_index) >= 0) {
            *out_result = D1L_CHANNEL_MUTATION_NAME_COLLISION;
            ret = ESP_ERR_INVALID_STATE;
        } else if (entry->channel_id == D1L_CHANNEL_PUBLIC_ID) {
            *out_result = D1L_CHANNEL_MUTATION_EXISTS;
            copy_info(entry, out_info);
            ret = ESP_OK;
        } else if (!mutation_counter_available()) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            fill_blob(&s_rollback_scratch);
            memcpy(entry->name, parsed.name, strlen(parsed.name) + 1U);
            entry->source = D1L_CHANNEL_SOURCE_URI_IMPORT;
            entry->imported_at_ms = now_ms();
            entry->updated_ms = entry->imported_at_ms;
            note_mutation();
            ret = persist_store_or_rollback(&s_rollback_scratch);
            if (ret == ESP_OK) {
                *out_result = D1L_CHANNEL_MUTATION_UPDATED;
                copy_info(&s_entries[secret_index], out_info);
            }
        }
    }
    d1l_store_lock_give(&s_store_lock);
    secure_zero(parsed.secret, sizeof(parsed.secret));
    return ret;
}

esp_err_t d1l_channel_store_update(
    uint64_t channel_id, const char *name, bool enabled, bool make_default,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_info)
{
    if (!out_result || channel_id == 0U || !input_name_is_valid(name) ||
        (make_default && !enabled)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = D1L_CHANNEL_MUTATION_NONE;
    if (out_info) {
        memset(out_info, 0, sizeof(*out_info));
    }
    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_store_lock_take(&s_store_lock);
    const int index = index_by_id(channel_id);
    if (index < 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (channel_id == D1L_CHANNEL_PUBLIC_ID &&
               (strcmp(name, D1L_CHANNEL_PUBLIC_NAME) != 0 || !enabled)) {
        *out_result = D1L_CHANNEL_MUTATION_PROTECTED;
        ret = ESP_ERR_NOT_SUPPORTED;
    } else if (index_by_name(name, index) >= 0) {
        *out_result = D1L_CHANNEL_MUTATION_NAME_COLLISION;
        ret = ESP_ERR_INVALID_STATE;
    } else {
        d1l_channel_entry_t *entry = &s_entries[index];
        const bool name_changed = strcmp(entry->name, name) != 0;
        const bool enabled_changed = entry->enabled != (enabled ? 1U : 0U);
        const bool default_changed = make_default && entry->is_default == 0U;
        const bool fallback_default = !enabled && entry->is_default != 0U;
        if (!name_changed && !enabled_changed && !default_changed &&
            !fallback_default) {
            *out_result = D1L_CHANNEL_MUTATION_EXISTS;
            copy_info(entry, out_info);
            ret = ESP_OK;
        } else if (!mutation_counter_available()) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            fill_blob(&s_rollback_scratch);
            memcpy(entry->name, name, strlen(name) + 1U);
            entry->enabled = enabled ? 1U : 0U;
            entry->updated_ms = now_ms();
            if (make_default) {
                make_only_default((size_t)index);
            } else if (fallback_default) {
                const int public_index = index_by_id(D1L_CHANNEL_PUBLIC_ID);
                if (public_index < 0) {
                    restore_blob(&s_rollback_scratch);
                    secure_zero(&s_rollback_scratch,
                                sizeof(s_rollback_scratch));
                    d1l_store_lock_give(&s_store_lock);
                    return ESP_ERR_INVALID_STATE;
                }
                make_only_default((size_t)public_index);
            }
            note_mutation();
            ret = persist_store_or_rollback(&s_rollback_scratch);
            if (ret == ESP_OK) {
                *out_result = D1L_CHANNEL_MUTATION_UPDATED;
                copy_info(&s_entries[index], out_info);
            }
        }
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

esp_err_t d1l_channel_store_select(
    uint64_t channel_id, d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_info)
{
    if (!out_result || channel_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = D1L_CHANNEL_MUTATION_NONE;
    if (out_info) {
        memset(out_info, 0, sizeof(*out_info));
    }
    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    const int index = index_by_id(channel_id);
    if (index < 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (s_entries[index].enabled == 0U) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (s_entries[index].is_default != 0U) {
        *out_result = D1L_CHANNEL_MUTATION_EXISTS;
        copy_info(&s_entries[index], out_info);
        ret = ESP_OK;
    } else if (!mutation_counter_available()) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        fill_blob(&s_rollback_scratch);
        s_entries[index].updated_ms = now_ms();
        make_only_default((size_t)index);
        note_mutation();
        ret = persist_store_or_rollback(&s_rollback_scratch);
        if (ret == ESP_OK) {
            *out_result = D1L_CHANNEL_MUTATION_UPDATED;
            copy_info(&s_entries[index], out_info);
        }
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

esp_err_t d1l_channel_store_remove(
    uint64_t channel_id, d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_info)
{
    if (!out_result || channel_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = D1L_CHANNEL_MUTATION_NONE;
    if (out_info) {
        memset(out_info, 0, sizeof(*out_info));
    }
    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_store_lock_take(&s_store_lock);
    const int index = index_by_id(channel_id);
    if (index < 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (channel_id == D1L_CHANNEL_PUBLIC_ID) {
        *out_result = D1L_CHANNEL_MUTATION_PROTECTED;
        ret = ESP_ERR_NOT_SUPPORTED;
    } else if (!mutation_counter_available()) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        d1l_channel_info_t removed;
        copy_info(&s_entries[index], &removed);
        const bool was_default = s_entries[index].is_default != 0U;
        fill_blob(&s_rollback_scratch);
        for (size_t i = (size_t)index; i + 1U < s_count; ++i) {
            s_entries[i] = s_entries[i + 1U];
        }
        s_count--;
        secure_zero(&s_entries[s_count], sizeof(s_entries[s_count]));
        if (was_default) {
            const int public_index = index_by_id(D1L_CHANNEL_PUBLIC_ID);
            if (public_index < 0) {
                restore_blob(&s_rollback_scratch);
                secure_zero(&s_rollback_scratch,
                            sizeof(s_rollback_scratch));
                d1l_store_lock_give(&s_store_lock);
                return ESP_ERR_INVALID_STATE;
            }
            make_only_default((size_t)public_index);
        }
        note_mutation();
        ret = persist_store_or_rollback(&s_rollback_scratch);
        if (ret == ESP_OK) {
            *out_result = D1L_CHANNEL_MUTATION_REMOVED;
            if (out_info) {
                *out_info = removed;
            }
        }
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

esp_err_t d1l_channel_store_note_message(uint64_t channel_id,
                                         uint32_t message_seq,
                                         bool unread)
{
    if (channel_id == 0U || message_seq == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_store_lock_take(&s_store_lock);
    const int index = index_by_id(channel_id);
    if (index < 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (message_seq <= s_entries[index].newest_message_seq) {
        ret = ESP_OK;
    } else if (!mutation_counter_available() ||
               (unread && s_entries[index].unread_count == UINT32_MAX)) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        fill_blob(&s_rollback_scratch);
        d1l_channel_entry_t *entry = &s_entries[index];
        entry->newest_message_seq = message_seq;
        if (unread) {
            entry->unread_count++;
        } else if (entry->unread_count == 0U) {
            entry->read_through_seq = message_seq;
        }
        entry->updated_ms = now_ms();
        note_mutation();
        ret = persist_store_or_rollback(&s_rollback_scratch);
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

esp_err_t d1l_channel_store_mark_all_read(uint64_t channel_id)
{
    if (channel_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_store_lock_take(&s_store_lock);
    const int index = index_by_id(channel_id);
    if (index < 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (s_entries[index].read_through_seq ==
                   s_entries[index].newest_message_seq &&
               s_entries[index].unread_count == 0U) {
        ret = ESP_OK;
    } else if (!mutation_counter_available()) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        fill_blob(&s_rollback_scratch);
        d1l_channel_entry_t *entry = &s_entries[index];
        entry->read_through_seq = entry->newest_message_seq;
        entry->unread_count = 0U;
        entry->updated_ms = now_ms();
        note_mutation();
        ret = persist_store_or_rollback(&s_rollback_scratch);
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

esp_err_t d1l_channel_store_reconcile_retained_rows(
    const d1l_channel_retained_row_t *rows, size_t row_count,
    const d1l_channel_message_generation_t *message_generation)
{
    if (!message_generation || message_generation->epoch == 0U ||
        message_generation->next_seq == 0U ||
        (row_count > 0U && !rows) ||
        row_count > D1L_CHANNEL_RETAINED_ROW_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t previous_seq = 0U;
    for (size_t i = 0U; i < row_count; ++i) {
        if (rows[i].channel_id == 0U || rows[i].message_seq == 0U ||
            rows[i].message_seq >= message_generation->next_seq ||
            rows[i].message_seq <= previous_seq) {
            return ESP_ERR_INVALID_ARG;
        }
        previous_seq = rows[i].message_seq;
    }

    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    uint32_t retained_newest[D1L_CHANNEL_STORE_CAPACITY] = {0};
    uint32_t derived_newest[D1L_CHANNEL_STORE_CAPACITY] = {0};
    uint32_t baseline_read_through[D1L_CHANNEL_STORE_CAPACITY] = {0};
    uint32_t retained_unread[D1L_CHANNEL_STORE_CAPACITY] = {0};
    for (size_t i = 0U; i < row_count; ++i) {
        if (!channel_available_in_release_profile(rows[i].channel_id)) {
            continue;
        }
        const int index = index_by_id(rows[i].channel_id);
        if (index < 0) {
            /* History for a removed channel remains identity-isolated until a
             * later confirmed purge; it must not poison configured cursors. */
            continue;
        }
        if (rows[i].message_seq > retained_newest[index]) {
            retained_newest[index] = rows[i].message_seq;
        }
    }

    const bool generation_known =
        s_message_epoch != 0U && s_message_next_seq != 0U;
    const bool same_message_generation = generation_known &&
        s_message_epoch == message_generation->epoch &&
        s_message_clear_lineage == message_generation->clear_lineage;
    const bool sequence_rewound = generation_known &&
        message_generation->next_seq < s_message_next_seq;
    bool unknown_out_of_domain = false;
    if (!generation_known) {
        for (size_t i = 0U; i < s_count; ++i) {
            if (!channel_available_in_release_profile(
                    s_entries[i].channel_id)) {
                continue;
            }
            if (s_entries[i].newest_message_seq >=
                    message_generation->next_seq ||
                s_entries[i].read_through_seq >=
                    message_generation->next_seq) {
                unknown_out_of_domain = true;
                break;
            }
        }
    }
    const bool true_generation_reset =
        (sequence_rewound && !same_message_generation) ||
        unknown_out_of_domain;
    const bool durability_rollback =
        sequence_rewound && same_message_generation;

    for (size_t i = 0U; i < s_count; ++i) {
        const d1l_channel_entry_t *entry = &s_entries[i];
        if (!channel_available_in_release_profile(entry->channel_id)) {
            continue;
        }
        if (true_generation_reset) {
            derived_newest[i] = retained_newest[i];
            baseline_read_through[i] = 0U;
        } else if (durability_rollback) {
            derived_newest[i] = retained_newest[i];
            baseline_read_through[i] =
                entry->read_through_seq < retained_newest[i] ?
                    entry->read_through_seq : retained_newest[i];
        } else {
            derived_newest[i] =
                retained_newest[i] > entry->newest_message_seq ?
                    retained_newest[i] : entry->newest_message_seq;
            baseline_read_through[i] = entry->read_through_seq;
        }
    }
    for (size_t i = 0U; i < row_count; ++i) {
        if (!channel_available_in_release_profile(rows[i].channel_id)) {
            continue;
        }
        const int index = index_by_id(rows[i].channel_id);
        if (index >= 0 && rows[i].received &&
            rows[i].message_seq > baseline_read_through[index]) {
            retained_unread[index]++;
        }
    }

    bool changed = s_message_epoch != message_generation->epoch ||
        s_message_next_seq != message_generation->next_seq ||
        s_message_clear_lineage != message_generation->clear_lineage;
    for (size_t i = 0U; i < s_count; ++i) {
        d1l_channel_entry_t *entry = &s_entries[i];
        if (!channel_available_in_release_profile(entry->channel_id)) {
            continue;
        }
        const uint32_t newest = derived_newest[i];
        const uint32_t unread = retained_unread[i];
        const uint32_t read_through = unread == 0U ?
            newest : baseline_read_through[i];
        if (entry->newest_message_seq != newest ||
            entry->read_through_seq != read_through ||
            entry->unread_count != unread) {
            changed = true;
            break;
        }
    }

    if (!changed) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_OK;
    }
    if (!mutation_counter_available()) {
        d1l_store_lock_give(&s_store_lock);
        return ESP_ERR_INVALID_STATE;
    }

    fill_blob(&s_rollback_scratch);
    const uint32_t reconciled_at_ms = now_ms();
    for (size_t i = 0U; i < s_count; ++i) {
        d1l_channel_entry_t *entry = &s_entries[i];
        if (!channel_available_in_release_profile(entry->channel_id)) {
            continue;
        }
        const uint32_t newest = derived_newest[i];
        const uint32_t unread = retained_unread[i];
        const uint32_t read_through = unread == 0U ?
            newest : baseline_read_through[i];
        if (entry->newest_message_seq != newest ||
            entry->read_through_seq != read_through ||
            entry->unread_count != unread) {
            entry->newest_message_seq = newest;
            entry->read_through_seq = read_through;
            entry->unread_count = unread;
            entry->updated_ms = reconciled_at_ms;
        }
    }
    s_message_epoch = message_generation->epoch;
    s_message_next_seq = message_generation->next_seq;
    s_message_clear_lineage = message_generation->clear_lineage;
    note_mutation();
    ret = persist_store_or_rollback(&s_rollback_scratch);
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

d1l_channel_store_stats_t d1l_channel_store_stats(void)
{
    d1l_store_lock_take(&s_store_lock);
    d1l_channel_store_stats_t stats = {
        .lineage = s_loaded ? s_lineage : 0U,
        .generation = s_loaded ? s_generation : 0U,
        .next_channel_id = s_loaded ? s_next_channel_id : 0U,
        .message_clear_lineage =
            s_loaded ? s_message_clear_lineage : 0U,
        .revision = s_loaded ? s_revision : 0U,
        .total_mutations = s_loaded ? s_total_mutations : 0U,
        .message_epoch = s_loaded ? s_message_epoch : 0U,
        .message_next_seq = s_loaded ? s_message_next_seq : 0U,
        .count = s_loaded ? s_count : 0U,
        .capacity = D1L_CHANNEL_STORE_CAPACITY,
        .load_status = s_load_status,
        .loaded = s_loaded,
    };
    d1l_store_lock_give(&s_store_lock);
    return stats;
}

esp_err_t d1l_channel_store_snapshot(
    d1l_channel_info_t *out_channels, size_t max_channels,
    size_t *out_count, uint64_t *out_active_channel_id,
    d1l_channel_store_stats_t *out_stats)
{
    if (!out_channels || max_channels == 0U || !out_count ||
        !out_active_channel_id || !out_stats) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0U;
    *out_active_channel_id = 0U;
    memset(out_stats, 0, sizeof(*out_stats));

    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        out_stats->capacity = D1L_CHANNEL_STORE_CAPACITY;
        out_stats->load_status = ret;
        return ret;
    }

    d1l_store_lock_take(&s_store_lock);
    const int active_index = default_index();
    if (!s_loaded || s_load_status != ESP_OK) {
        ret = s_load_status != ESP_OK ? s_load_status : ESP_ERR_INVALID_STATE;
    } else if (active_index < 0 || s_entries[active_index].enabled == 0U) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        const size_t count = s_count < max_channels ? s_count : max_channels;
        for (size_t i = 0U; i < count; ++i) {
            copy_info(&s_entries[i], &out_channels[i]);
        }
        *out_count = count;
        *out_active_channel_id = s_entries[active_index].channel_id;
        *out_stats = (d1l_channel_store_stats_t){
            .lineage = s_lineage,
            .generation = s_generation,
            .next_channel_id = s_next_channel_id,
            .message_clear_lineage = s_message_clear_lineage,
            .revision = s_revision,
            .total_mutations = s_total_mutations,
            .message_epoch = s_message_epoch,
            .message_next_seq = s_message_next_seq,
            .count = s_count,
            .capacity = D1L_CHANNEL_STORE_CAPACITY,
            .load_status = s_load_status,
            .loaded = s_loaded,
        };
        ret = ESP_OK;
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

size_t d1l_channel_store_copy(d1l_channel_info_t *out_channels,
                              size_t max_channels)
{
    if (!out_channels || max_channels == 0U || ensure_loaded() != ESP_OK) {
        return 0U;
    }
    d1l_store_lock_take(&s_store_lock);
    const size_t count = s_count < max_channels ? s_count : max_channels;
    for (size_t i = 0U; i < count; ++i) {
        copy_info(&s_entries[i], &out_channels[i]);
    }
    d1l_store_lock_give(&s_store_lock);
    return count;
}

bool d1l_channel_store_find(uint64_t channel_id,
                            d1l_channel_info_t *out_info)
{
    if (channel_id == 0U || !out_info || ensure_loaded() != ESP_OK) {
        return false;
    }
    d1l_store_lock_take(&s_store_lock);
    const int index = index_by_id(channel_id);
    if (index >= 0) {
        copy_info(&s_entries[index], out_info);
    } else {
        memset(out_info, 0, sizeof(*out_info));
    }
    d1l_store_lock_give(&s_store_lock);
    return index >= 0;
}

bool d1l_channel_store_find_default(d1l_channel_info_t *out_info)
{
    if (!out_info || ensure_loaded() != ESP_OK) {
        return false;
    }
    d1l_store_lock_take(&s_store_lock);
    const int index = default_index();
    if (index >= 0) {
        copy_info(&s_entries[index], out_info);
    } else {
        memset(out_info, 0, sizeof(*out_info));
    }
    d1l_store_lock_give(&s_store_lock);
    return index >= 0;
}

esp_err_t d1l_channel_store_copy_protocol_key(
    uint64_t channel_id, d1l_channel_protocol_key_t *out_key)
{
    if (channel_id == 0U || !out_key) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_key, 0, sizeof(*out_key));
    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_store_lock_take(&s_store_lock);
    const int index = index_by_id(channel_id);
    if (index < 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (s_entries[index].enabled == 0U) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        copy_protocol_key(&s_entries[index], out_key);
        ret = ESP_OK;
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

size_t d1l_channel_store_copy_hash_matches(
    uint8_t channel_hash, d1l_channel_protocol_key_t *out_keys,
    size_t max_keys)
{
    if (!out_keys || max_keys == 0U || ensure_loaded() != ESP_OK) {
        return 0U;
    }
    d1l_store_lock_take(&s_store_lock);
    size_t count = 0U;
    for (size_t i = 0U; i < s_count && count < max_keys; ++i) {
        if (s_entries[i].enabled != 0U &&
            s_entries[i].channel_hash == channel_hash) {
            copy_protocol_key(&s_entries[i], &out_keys[count++]);
        }
    }
    d1l_store_lock_give(&s_store_lock);
    return count;
}

size_t d1l_channel_store_copy_rx_hash_matches(
    uint8_t channel_hash, d1l_channel_protocol_key_t *out_keys,
    size_t max_keys)
{
    if (!out_keys || max_keys == 0U || ensure_loaded() != ESP_OK) {
        return 0U;
    }
    d1l_store_lock_take(&s_store_lock);
    size_t count = 0U;
    for (size_t i = 0U; i < s_count && count < max_keys; ++i) {
        if (!channel_available_in_release_profile(s_entries[i].channel_id)) {
            continue;
        }
        if (s_entries[i].enabled != 0U &&
            s_entries[i].channel_hash == channel_hash) {
            copy_protocol_key(&s_entries[i], &out_keys[count++]);
        }
    }
    d1l_store_lock_give(&s_store_lock);
    return count;
}

esp_err_t d1l_channel_store_find_unique_hash(
    uint8_t channel_hash, d1l_channel_protocol_key_t *out_key)
{
    if (!out_key) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_key, 0, sizeof(*out_key));
    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_store_lock_take(&s_store_lock);
    const d1l_channel_entry_t *match = NULL;
    size_t matches = 0U;
    for (size_t i = 0U; i < s_count; ++i) {
        if (s_entries[i].enabled != 0U &&
            s_entries[i].channel_hash == channel_hash) {
            match = &s_entries[i];
            matches++;
        }
    }
    if (matches == 1U) {
        copy_protocol_key(match, out_key);
        ret = ESP_OK;
    } else {
        ret = matches == 0U ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}

static bool encode_uri_name(const char *name, char *dest, size_t dest_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0U;
    for (size_t i = 0U; name[i] != '\0'; ++i) {
        const unsigned char value = (unsigned char)name[i];
        if ((value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '_' ||
            value == '.') {
            if (out + 1U >= dest_size) {
                return false;
            }
            dest[out++] = (char)value;
        } else if (value == ' ') {
            if (out + 1U >= dest_size) {
                return false;
            }
            dest[out++] = '+';
        } else {
            if (out + 3U >= dest_size) {
                return false;
            }
            dest[out++] = '%';
            dest[out++] = hex[(value >> 4U) & 0x0fU];
            dest[out++] = hex[value & 0x0fU];
        }
    }
    dest[out] = '\0';
    return true;
}

esp_err_t d1l_channel_store_export_share_uri(uint64_t channel_id, char *dest,
                                             size_t dest_size)
{
    if (channel_id == 0U || !dest || dest_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    dest[0] = '\0';
    esp_err_t ret = ensure_loaded();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_store_lock_take(&s_store_lock);
    const int index = index_by_id(channel_id);
    if (index < 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else {
        const d1l_channel_entry_t *entry = &s_entries[index];
        if (entry->secret_len != D1L_CHANNEL_SECRET_128_LEN) {
            d1l_store_lock_give(&s_store_lock);
            return ESP_ERR_NOT_SUPPORTED;
        }
        char encoded_name[D1L_CHANNEL_NAME_LEN * 3U] = {0};
        char secret_hex[D1L_CHANNEL_SECRET_MAX_LEN * 2U + 1U] = {0};
        static const char hex[] = "0123456789abcdef";
        if (!encode_uri_name(entry->name, encoded_name,
                             sizeof(encoded_name))) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            for (size_t i = 0U; i < entry->secret_len; ++i) {
                secret_hex[i * 2U] = hex[(entry->secret[i] >> 4U) & 0x0fU];
                secret_hex[i * 2U + 1U] = hex[entry->secret[i] & 0x0fU];
            }
            const int written = snprintf(
                dest, dest_size,
                D1L_CHANNEL_URI_SCHEME "name=%s&secret=%s", encoded_name,
                secret_hex);
            ret = written < 0 || (size_t)written >= dest_size
                      ? ESP_ERR_INVALID_SIZE
                      : ESP_OK;
            if (ret != ESP_OK) {
                dest[0] = '\0';
            }
        }
        secure_zero(secret_hex, sizeof(secret_hex));
    }
    d1l_store_lock_give(&s_store_lock);
    return ret;
}
