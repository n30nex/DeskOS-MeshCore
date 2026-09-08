#pragma once

#include "mesh/channel_store.h"
#include "mesh/contact_store.h"
#include "mesh/user_text.h"
#include "storage/retained_store_scheduler.h"

#define D1L_DRAFT_CAPACITY (D1L_CHANNEL_STORE_CAPACITY + D1L_CONTACT_STORE_CAPACITY)
#define D1L_DRAFT_TEXT_BYTES (D1L_USER_TEXT_MAX_BYTES * 4U + 1U)

/* Channel history identity and the full DM public key prevent a renamed or
 * replaced conversation from inheriting another recipient's unsent text. */
typedef struct {
    uint64_t channel_id;
    uint64_t history_key;
    char public_key[D1L_NODE_PUBLIC_KEY_HEX_LEN];
} d1l_draft_key_t;

bool d1l_draft_key_channel(const d1l_channel_info_t *channel, d1l_draft_key_t *key);
bool d1l_draft_key_contact(const d1l_contact_entry_t *contact, d1l_draft_key_t *key);
void d1l_draft_store_init(void);
esp_err_t d1l_draft_store_get(const d1l_draft_key_t *key, char *text, size_t capacity);
esp_err_t d1l_draft_store_set(const d1l_draft_key_t *key, const char *text);
esp_err_t d1l_draft_store_flush(void);
esp_err_t d1l_draft_store_flush_if_due(void);
void d1l_draft_store_observe(d1l_retained_store_observation_t *observation);
const char *d1l_draft_store_save_status(void);
