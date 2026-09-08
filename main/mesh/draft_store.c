#include "draft_store.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include "app/settings_envelope.h"
#include "app/settings_model.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "mesh/store_lock.h"
#include "storage/retained_blob_store.h"

#define DRAFT_MAGIC UINT32_C(0x44524654)
#define DRAFT_VERSION 1U
#define DRAFT_KEY "drafts_v1"
#define DRAFT_STORE D1L_RETAINED_BLOB_STORE_DRAFTS

typedef struct {
    d1l_draft_key_t key;
    char text[D1L_DRAFT_TEXT_BYTES];
} draft_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t owner_public_key[32];
    draft_entry_t entries[D1L_DRAFT_CAPACITY];
    uint32_t checksum;
} draft_blob_t;

static draft_blob_t s_drafts EXT_RAM_BSS_ATTR;
/* Only the retained worker uses this buffer; no SD operation holds s_lock. */
static draft_blob_t s_io EXT_RAM_BSS_ATTR;
static d1l_store_lock_t s_lock = D1L_STORE_LOCK_INITIALIZER;
static d1l_store_lock_t s_io_lock = D1L_STORE_LOCK_INITIALIZER;
static bool s_touched[D1L_DRAFT_CAPACITY];
static bool s_initialized, s_loaded, s_dirty;
static uint32_t s_generation, s_commits, s_failures;
static uint64_t s_revision;
static int64_t s_changed_us, s_attempt_us;
static esp_err_t s_error;

static bool key_present(const d1l_draft_key_t *key)
{
    return key->channel_id != 0U || key->public_key[0] != '\0';
}

static bool key_valid(const d1l_draft_key_t *key)
{
    if (!key) return false;
    if (key->channel_id != 0U) return key->history_key != 0U && key->public_key[0] == '\0';
    if (key->history_key != 0U || key->public_key[64] != '\0') return false;
    for (size_t i = 0U; i < 64U; ++i) {
        if (!isxdigit((unsigned char)key->public_key[i])) return false;
    }
    return true;
}

static bool key_equal(const d1l_draft_key_t *a, const d1l_draft_key_t *b)
{
    return a->channel_id == b->channel_id && a->history_key == b->history_key &&
        strcmp(a->public_key, b->public_key) == 0;
}

static bool key_current(const d1l_draft_key_t *key)
{
    if (key->channel_id != 0U) {
        d1l_channel_info_t channel = {0};
        return d1l_channel_store_find(key->channel_id, &channel) &&
            channel.history_key == key->history_key;
    }
    d1l_contact_entry_t contact = {0};
    return d1l_contact_store_find_by_public_key(key->public_key, &contact);
}

static bool text_valid(const char *text)
{
    if (!text) return false;
    const char *end = memchr(text, '\0', D1L_DRAFT_TEXT_BYTES);
    if (!end) return false;
    if (end == text) return true;
    const d1l_user_text_info_t info = d1l_user_text_validate_display_span(
        (const uint8_t *)text, (size_t)(end - text));
    return info.result == D1L_USER_TEXT_OK && info.character_count <= D1L_USER_TEXT_MAX_BYTES;
}

bool d1l_draft_key_channel(const d1l_channel_info_t *channel, d1l_draft_key_t *key)
{
    if (!channel || !key) return false;
    memset(key, 0, sizeof(*key));
    key->channel_id = channel->channel_id;
    key->history_key = channel->history_key;
    return key_valid(key);
}

bool d1l_draft_key_contact(const d1l_contact_entry_t *contact, d1l_draft_key_t *key)
{
    if (!contact || !key) return false;
    memset(key, 0, sizeof(*key));
    if (contact->public_key_hex[64] != '\0') return false;
    for (size_t i = 0U; i < 64U; ++i) {
        key->public_key[i] = (char)tolower((unsigned char)contact->public_key_hex[i]);
    }
    return key_valid(key);
}

static int entry_index(draft_blob_t *blob, const d1l_draft_key_t *key)
{
    for (size_t i = 0U; i < D1L_DRAFT_CAPACITY; ++i) {
        if (key_present(&blob->entries[i].key) && key_equal(&blob->entries[i].key, key)) return (int)i;
    }
    return -1;
}

static int free_index(draft_blob_t *blob)
{
    for (size_t i = 0U; i < D1L_DRAFT_CAPACITY; ++i) {
        if (!key_present(&blob->entries[i].key)) return (int)i;
    }
    return -1;
}

void d1l_draft_store_init(void)
{
    d1l_store_lock_take(&s_io_lock);
    d1l_store_lock_take(&s_lock);
    memset(&s_drafts, 0, sizeof(s_drafts));
    memset(&s_io, 0, sizeof(s_io));
    memset(s_touched, 0, sizeof(s_touched));
    s_initialized = true;
    s_loaded = s_dirty = false;
    s_generation = s_commits = s_failures = 0U;
    s_revision = 0U;
    s_changed_us = s_attempt_us = 0;
    s_error = ESP_OK;
    d1l_store_lock_give(&s_lock);
    d1l_store_lock_give(&s_io_lock);
}

esp_err_t d1l_draft_store_get(const d1l_draft_key_t *key, char *text, size_t capacity)
{
    if (!key_valid(key) || !text || capacity == 0U) return ESP_ERR_INVALID_ARG;
    text[0] = '\0';
    d1l_store_lock_take(&s_lock);
    const int index = entry_index(&s_drafts, key);
    esp_err_t ret = ESP_OK;
    d1l_retained_blob_store_backend_state_t backend = {0};
    (void)d1l_retained_blob_store_backend_state(DRAFT_STORE, &backend);
    if (!s_initialized) ret = ESP_ERR_NOT_FINISHED;
    else if (backend.enabled && (!s_loaded || s_generation != backend.generation) &&
        (index < 0 || !s_touched[index])) {
        ret = s_error != ESP_OK ? s_error : ESP_ERR_NOT_FINISHED;
    } else if (index >= 0) {
        const size_t length = strlen(s_drafts.entries[index].text);
        if (length >= capacity) ret = ESP_ERR_INVALID_SIZE;
        else memcpy(text, s_drafts.entries[index].text, length + 1U);
    } else if (free_index(&s_drafts) < 0) {
        ret = ESP_ERR_NO_MEM;
    }
    d1l_store_lock_give(&s_lock);
    return ret;
}

esp_err_t d1l_draft_store_set(const d1l_draft_key_t *key, const char *text)
{
    if (!key_valid(key) || !text_valid(text)) return ESP_ERR_INVALID_ARG;
    d1l_store_lock_take(&s_lock);
    int index = entry_index(&s_drafts, key);
    if (index >= 0 && strcmp(s_drafts.entries[index].text, text) == 0) {
        d1l_store_lock_give(&s_lock);
        return ESP_OK;
    }
    if (index < 0) index = free_index(&s_drafts);
    if (!s_initialized || index < 0 || s_revision == UINT64_MAX) {
        d1l_store_lock_give(&s_lock);
        return index < 0 ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    draft_entry_t *entry = &s_drafts.entries[index];
    memset(entry, 0, sizeof(*entry));
    entry->key = *key;
    memcpy(entry->text, text, strlen(text));
    s_touched[index] = true; /* Empty text is a deletion until SD confirms it. */
    s_dirty = true;
    ++s_revision;
    s_changed_us = esp_timer_get_time();
    d1l_store_lock_give(&s_lock);
    return ESP_OK;
}

static bool generation_matches(uint32_t generation)
{
    d1l_retained_blob_store_backend_state_t backend = {0};
    return d1l_retained_blob_store_backend_state(DRAFT_STORE, &backend) &&
        backend.enabled && backend.generation == generation;
}

static esp_err_t load_blob(uint32_t generation, const uint8_t owner[32])
{
    memset(&s_io, 0, sizeof(s_io));
    size_t length = sizeof(s_io);
    esp_err_t ret = d1l_retained_blob_store_read_sd_primary(DRAFT_STORE, DRAFT_KEY, &s_io, &length);
    if (!generation_matches(generation)) return ESP_ERR_INVALID_STATE;
    if (ret == ESP_ERR_NOT_FOUND) {
        memset(&s_io, 0, sizeof(s_io));
        return ESP_OK;
    }
    if (ret != ESP_OK) return ret;
    if (length != sizeof(s_io) || s_io.magic != DRAFT_MAGIC || s_io.version != DRAFT_VERSION ||
        s_io.checksum != d1l_settings_envelope_checksum(&s_io, offsetof(draft_blob_t, checksum))) return ESP_ERR_INVALID_SIZE;
    /* An older rollback image cannot fence a store it does not know about.
     * Binding drafts to the local public identity also protects a reset on
     * that image and cards moved between devices. */
    if (memcmp(s_io.owner_public_key, owner, 32U) != 0) {
        memset(&s_io, 0, sizeof(s_io));
        return ESP_OK;
    }
    for (size_t i = 0U; i < D1L_DRAFT_CAPACITY; ++i) {
        draft_entry_t *entry = &s_io.entries[i];
        if (!key_present(&entry->key)) continue;
        if (!key_valid(&entry->key) || !text_valid(entry->text)) return ESP_ERR_INVALID_ARG;
        for (size_t j = 0U; j < i; ++j) {
            if (key_present(&s_io.entries[j].key) && key_equal(&s_io.entries[j].key, &entry->key)) return ESP_ERR_INVALID_ARG;
        }
        if (!key_current(&entry->key)) memset(entry, 0, sizeof(*entry));
    }
    return ESP_OK;
}

static esp_err_t flush_store(bool force)
{
    if (!d1l_store_lock_try_take(&s_io_lock)) return ESP_ERR_NOT_FINISHED;
    d1l_retained_blob_store_backend_state_t backend = {0};
    (void)d1l_retained_blob_store_backend_state(DRAFT_STORE, &backend);
    d1l_store_lock_take(&s_lock);
    const bool reconcile = !s_loaded || s_generation != backend.generation;
    const int64_t now = esp_timer_get_time();
    const bool due = force || (now - s_attempt_us >= INT64_C(2000000) &&
        (reconcile || now - s_changed_us >= INT64_C(1500000)));
    if (!s_initialized || !backend.enabled || (!reconcile && !s_dirty) || !due) {
        d1l_store_lock_give(&s_lock);
        d1l_store_lock_give(&s_io_lock);
        return ESP_OK;
    }
    s_attempt_us = now;
    d1l_store_lock_give(&s_lock);
    d1l_settings_t settings = {0};
    esp_err_t ret = d1l_settings_public_snapshot(&settings);
    if (ret == ESP_OK && !settings.identity_ready) ret = ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK && reconcile) ret = load_blob(backend.generation, settings.identity_public_key);
    d1l_store_lock_take(&s_lock);
    if (ret == ESP_OK && !generation_matches(backend.generation)) ret = ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK && reconcile) {
        bool merged_touched[D1L_DRAFT_CAPACITY] = {false};
        for (size_t i = 0U; i < D1L_DRAFT_CAPACITY; ++i) {
            if (!s_touched[i]) continue;
            int target = entry_index(&s_io, &s_drafts.entries[i].key);
            if (target < 0) target = free_index(&s_io);
            if (target < 0) { ret = ESP_ERR_NO_MEM; break; }
            s_io.entries[target] = s_drafts.entries[i];
            merged_touched[target] = true;
        }
        if (ret == ESP_OK) {
            s_drafts = s_io;
            memcpy(s_touched, merged_touched, sizeof(s_touched));
            s_loaded = true;
            s_generation = backend.generation;
        }
    }
    const uint64_t revision = s_revision;
    const bool write = ret == ESP_OK && s_dirty;
    if (write) {
        s_io = s_drafts;
        s_io.magic = DRAFT_MAGIC;
        s_io.version = DRAFT_VERSION;
        memcpy(s_io.owner_public_key, settings.identity_public_key, sizeof(s_io.owner_public_key));
        for (size_t i = 0U; i < D1L_DRAFT_CAPACITY; ++i) {
            if (s_io.entries[i].text[0] == '\0') memset(&s_io.entries[i], 0, sizeof(s_io.entries[i]));
        }
        s_io.checksum = d1l_settings_envelope_checksum(&s_io, offsetof(draft_blob_t, checksum));
    }
    d1l_store_lock_give(&s_lock);
    if (write) ret = d1l_retained_blob_store_write_sd_primary_guarded(
        DRAFT_STORE, DRAFT_KEY, &s_io, sizeof(s_io), backend.generation);
    if (ret == ESP_OK && !generation_matches(backend.generation)) ret = ESP_ERR_INVALID_STATE;
    d1l_store_lock_take(&s_lock);
    s_error = ret;
    if (ret != ESP_OK) ++s_failures;
    else if (write) {
        ++s_commits;
        if (s_revision == revision) {
            s_drafts = s_io;
            memset(s_touched, 0, sizeof(s_touched));
            s_dirty = false;
        }
    }
    d1l_store_lock_give(&s_lock);
    memset(&s_io, 0, sizeof(s_io));
    d1l_store_lock_give(&s_io_lock);
    return ret;
}

esp_err_t d1l_draft_store_flush(void) { return flush_store(true); }
esp_err_t d1l_draft_store_flush_if_due(void) { return flush_store(false); }

void d1l_draft_store_observe(d1l_retained_store_observation_t *observation)
{
    if (!observation) return;
    d1l_retained_blob_store_backend_state_t backend = {0};
    (void)d1l_retained_blob_store_backend_state(DRAFT_STORE, &backend);
    d1l_store_lock_take(&s_lock);
    *observation = (d1l_retained_store_observation_t) {
        .revision = s_revision, .commit_count = s_commits, .failure_count = s_failures,
        .dirty = s_dirty, .reconcile_pending = s_initialized && backend.enabled &&
            (!s_loaded || s_generation != backend.generation),
    };
    d1l_store_lock_give(&s_lock);
}

const char *d1l_draft_store_save_status(void)
{
    d1l_retained_blob_store_backend_state_t backend = {0};
    (void)d1l_retained_blob_store_backend_state(DRAFT_STORE, &backend);
    d1l_store_lock_take(&s_lock);
    const char *status = !backend.enabled ? "Draft kept until restart (no SD)" :
        s_error != ESP_OK ? "Draft not saved to SD" :
        !s_loaded || s_generation != backend.generation ? "Loading saved drafts..." :
        s_dirty ? "Saving draft..." : "Draft saved on SD";
    d1l_store_lock_give(&s_lock);
    return status;
}
