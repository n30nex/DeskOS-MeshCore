#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mesh/draft_store.h"
#include "mesh/store_lock.h"
#include "storage/retained_blob_store.h"
#include "ui/compose_text.h"
#include "app/settings_model.h"

static unsigned char saved[100000];
static size_t saved_length;
static bool sd_enabled, fail_write, contact_exists = true;
static uint32_t generation = 1U;
static uint64_t channel_history = 101U;
static int64_t now_us = 10000000;
static unsigned writes;
static void (*during_write)(void), (*during_read)(void);
static d1l_draft_key_t channel_key, dm_key;
static uint8_t owner_identity = 0xa5U;

esp_err_t d1l_settings_public_snapshot(d1l_settings_t *out) {
    memset(out, 0, sizeof(*out)); out->identity_ready = true;
    memset(out->identity_public_key, owner_identity, sizeof(out->identity_public_key));
    return ESP_OK;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { s->value = 0U; return s; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    if (s->value && ticks == 0U) return 0;
    assert(!s->value); s->value = 1U; return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { assert(s->value); s->value = 0U; return pdTRUE; }
int64_t esp_timer_get_time(void) { return now_us; }
bool d1l_retained_blob_store_backend_state(d1l_retained_blob_store_id_t id,
    d1l_retained_blob_store_backend_state_t *out) {
    assert(id == D1L_RETAINED_BLOB_STORE_DRAFTS);
    *out = (d1l_retained_blob_store_backend_state_t){.enabled = sd_enabled, .generation = generation};
    return true;
}
esp_err_t d1l_retained_blob_store_read_sd_primary(d1l_retained_blob_store_id_t id,
    const char *key, void *out, size_t *length) {
    assert(id == D1L_RETAINED_BLOB_STORE_DRAFTS && strcmp(key, "drafts_v1") == 0);
    if (during_read) { void (*hook)(void) = during_read; during_read = NULL; hook(); }
    if (!saved_length) return ESP_ERR_NOT_FOUND;
    assert(*length >= saved_length); memcpy(out, saved, saved_length); *length = saved_length;
    return ESP_OK;
}
esp_err_t d1l_retained_blob_store_write_sd_primary_guarded(d1l_retained_blob_store_id_t id,
    const char *key, const void *data, size_t length, uint32_t expected) {
    assert(id == D1L_RETAINED_BLOB_STORE_DRAFTS && strcmp(key, "drafts_v1") == 0);
    assert(expected == generation && sd_enabled && length <= sizeof(saved));
    if (fail_write) return ESP_FAIL;
    memcpy(saved, data, length); saved_length = length; ++writes;
    if (during_write) { void (*hook)(void) = during_write; during_write = NULL; hook(); }
    return ESP_OK;
}
bool d1l_channel_store_find(uint64_t id, d1l_channel_info_t *out) {
    if (id != 1U) return false;
    *out = (d1l_channel_info_t){.channel_id = id, .history_key = channel_history};
    return true;
}
bool d1l_contact_store_find_by_public_key(const char *key, d1l_contact_entry_t *out) {
    if (!contact_exists || strcmp(key, dm_key.public_key) != 0) return false;
    memset(out, 0, sizeof(*out)); strcpy(out->public_key_hex, key); return true;
}
static void expect_text(const d1l_draft_key_t *key, const char *expected) {
    char actual[D1L_DRAFT_TEXT_BYTES];
    assert(d1l_draft_store_get(key, actual, sizeof(actual)) == ESP_OK);
    assert(strcmp(actual, expected) == 0);
}
static void edit_during_io(void) { assert(d1l_draft_store_set(&dm_key, "newer typing") == ESP_OK); }
static void switch_card(void) { ++generation; saved_length = 0U; }

int main(void) {
    d1l_channel_info_t channel = {.channel_id = 1U, .history_key = 101U};
    d1l_contact_entry_t contact = {0};
    memset(contact.public_key_hex, 'A', 64U); contact.public_key_hex[64] = '\0';
    assert(d1l_draft_key_channel(&channel, &channel_key));
    assert(d1l_draft_key_contact(&contact, &dm_key));
    assert(dm_key.public_key[0] == 'a');
    d1l_draft_store_init();
    expect_text(&channel_key, "");
    assert(d1l_draft_store_set(&channel_key, "channel draft") == ESP_OK);
    assert(d1l_draft_store_set(&dm_key, "private draft") == ESP_OK);
    expect_text(&channel_key, "channel draft"); expect_text(&dm_key, "private draft");
    assert(strstr(d1l_draft_store_save_status(), "no SD"));
    assert(d1l_draft_store_flush() == ESP_OK && writes == 0U);
    sd_enabled = true;
    assert(d1l_draft_store_flush() == ESP_OK && writes == 1U);
    d1l_draft_store_init();
    char text[D1L_DRAFT_TEXT_BYTES];
    assert(d1l_draft_store_get(&dm_key, text, sizeof(text)) == ESP_ERR_NOT_FINISHED);
    assert(d1l_draft_store_flush() == ESP_OK && writes == 1U);
    expect_text(&dm_key, "private draft"); expect_text(&channel_key, "channel draft");

    /* Failed writes retain the new RAM draft and leave the saved copy intact. */
    fail_write = true;
    assert(d1l_draft_store_set(&dm_key, "unsaved replacement") == ESP_OK);
    assert(d1l_draft_store_flush() == ESP_FAIL);
    expect_text(&dm_key, "unsaved replacement");
    assert(strstr(d1l_draft_store_save_status(), "not saved"));
    fail_write = false;
    assert(d1l_draft_store_flush() == ESP_OK);
    d1l_draft_store_init(); assert(d1l_draft_store_flush() == ESP_OK);
    expect_text(&dm_key, "unsaved replacement");

    /* Clearing offline is a tombstone, so a late card cannot revive it. */
    sd_enabled = false; ++generation;
    assert(d1l_draft_store_set(&dm_key, "") == ESP_OK);
    sd_enabled = true; ++generation;
    assert(d1l_draft_store_flush() == ESP_OK);
    d1l_draft_store_init(); assert(d1l_draft_store_flush() == ESP_OK);
    expect_text(&dm_key, ""); expect_text(&channel_key, "channel draft");

    /* No I/O holds the data lock, and writes cannot clear a newer edit. */
    assert(d1l_draft_store_set(&dm_key, "snapshot") == ESP_OK);
    during_write = edit_during_io;
    assert(d1l_draft_store_flush() == ESP_OK);
    expect_text(&dm_key, "newer typing");
    d1l_retained_store_observation_t observation;
    d1l_draft_store_observe(&observation); assert(observation.dirty);
    assert(d1l_draft_store_flush() == ESP_OK);
    d1l_draft_store_init(); during_read = edit_during_io;
    assert(d1l_draft_store_flush() == ESP_OK); expect_text(&dm_key, "newer typing");

    /* Generation changes never claim a write to a different card succeeded. */
    assert(d1l_draft_store_set(&dm_key, "keep across interrupted save") == ESP_OK);
    during_write = switch_card;
    assert(d1l_draft_store_flush() == ESP_ERR_INVALID_STATE);
    expect_text(&dm_key, "keep across interrupted save");
    assert(d1l_draft_store_flush() == ESP_OK);

    /* New history identity and a removed full-key contact cannot inherit drafts. */
    ++channel_history; contact_exists = false;
    d1l_draft_store_init(); assert(d1l_draft_store_flush() == ESP_OK);
    expect_text(&channel_key, ""); expect_text(&dm_key, "");
    contact_exists = true; channel_history = 101U;

    /* Corrupt or future data is preserved, never overwritten with an empty store. */
    saved[0] ^= 1U;
    unsigned before = writes;
    d1l_draft_store_init(); assert(d1l_draft_store_flush() != ESP_OK);
    assert(d1l_draft_store_set(&dm_key, "RAM survives corrupt storage") == ESP_OK);
    assert(d1l_draft_store_flush() != ESP_OK && writes == before);
    expect_text(&dm_key, "RAM survives corrupt storage");
    saved_length = 0U; ++generation; assert(d1l_draft_store_flush() == ESP_OK);

    char unicode[D1L_DRAFT_TEXT_BYTES];
    for (size_t i = 0U; i < 138U; ++i) memcpy(unicode + i * 4U, "\xf0\x9f\x98\x80", 4U);
    unicode[552] = '\0';
    assert(d1l_draft_store_set(&dm_key, unicode) == ESP_OK);
    assert(d1l_draft_store_flush() == ESP_OK);
    d1l_draft_store_init(); assert(d1l_draft_store_flush() == ESP_OK);
    expect_text(&dm_key, unicode);
    ++owner_identity;
    d1l_draft_store_init(); assert(d1l_draft_store_flush() == ESP_OK);
    expect_text(&dm_key, ""); /* A reset on older firmware also changes identity. */
    --owner_identity;
    d1l_draft_store_init(); assert(d1l_draft_store_flush() == ESP_OK);
    expect_text(&dm_key, unicode);
    assert(d1l_draft_store_set(&dm_key, "\xff") == ESP_ERR_INVALID_ARG);
    assert(d1l_draft_store_set(&dm_key, "\x01") == ESP_ERR_INVALID_ARG);
    assert(d1l_draft_store_set(&dm_key, "line one\nline two") == ESP_OK);
    assert(d1l_user_text_validate("line one\nline two").result != D1L_USER_TEXT_OK);
    before = writes;
    now_us += 1000000;
    assert(d1l_draft_store_flush_if_due() == ESP_OK && writes == before);
    now_us += 2000000;
    assert(d1l_draft_store_flush_if_due() == ESP_OK && writes == before + 1U);

    sd_enabled = false;
    d1l_draft_store_init();
    for (size_t i = 0U; i < D1L_DRAFT_CAPACITY; ++i) {
        d1l_draft_key_t key = {.channel_id = i + 1U, .history_key = i + 101U};
        assert(d1l_draft_store_set(&key, "keep every draft") == ESP_OK);
    }
    d1l_draft_key_t extra = {.channel_id = D1L_DRAFT_CAPACITY + 1U, .history_key = 999U};
    assert(d1l_draft_store_get(&extra, text, sizeof(text)) == ESP_ERR_NO_MEM);
    assert(d1l_draft_store_set(&extra, "must not evict") == ESP_ERR_NO_MEM);
    expect_text(&channel_key, "keep every draft");

    char quote[139];
    assert(d1l_compose_quote(quote, sizeof(quote), "Alice", "message"));
    assert(strcmp(quote, "> Alice: message | ") == 0);
    assert(d1l_compose_text_fits("My reply", quote));
    char full[139]; memset(full, 'a', 138U); full[138] = '\0';
    assert(!d1l_compose_text_fits(full, quote));
    assert(d1l_compose_quote(quote, sizeof(quote), "Alice", full));
    assert(strstr(quote, "... | ") && d1l_user_text_validate(quote).result == D1L_USER_TEXT_OK);
    assert(!d1l_compose_quote(quote, 5U, "Alice", full) && quote[0] == '\0');
    assert(d1l_compose_clipboard_copy("private \xc3\xa9"));
    assert(!d1l_compose_clipboard_copy("\xff"));
    assert(strcmp(d1l_compose_clipboard_text(), "private \xc3\xa9") == 0);
    assert(d1l_compose_clipboard_copy(d1l_compose_clipboard_text() + 8U));
    assert(strcmp(d1l_compose_clipboard_text(), "\xc3\xa9") == 0);
    d1l_compose_clipboard_clear(); assert(d1l_compose_clipboard_text()[0] == '\0');
    puts("PASS: draft persistence, failure recovery, late SD, identity boundaries, Unicode, quote and clipboard");
    return 0;
}
