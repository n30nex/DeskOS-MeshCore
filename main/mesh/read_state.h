#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "mesh/contact_store.h"
#include "mesh/dm_store.h"

#define D1L_READ_STATE_DM_THREAD_CAPACITY 16U

typedef struct {
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    uint32_t last_read_seq;
} d1l_read_state_persisted_dm_cursor_t;

typedef struct {
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    uint32_t last_read_seq;
    uint32_t newest_rx_seq;
    uint32_t unread_count;
    bool muted;
} d1l_read_state_dm_thread_t;

typedef struct {
    uint32_t last_public_read_seq;
    uint32_t last_dm_read_seq;
    uint32_t newest_public_rx_seq;
    uint32_t newest_dm_rx_seq;
    uint32_t public_unread_count;
    uint32_t dm_unread_count;
    uint32_t muted_dm_unread_count;
    uint32_t dm_thread_count;
    uint32_t persisted_dm_cursor_count;
    uint32_t persisted_dm_cursor_capacity;
    uint32_t mark_read_count;
    uint32_t accepted_sd_backend_generation;
    uint32_t sd_backend_generation;
    uint64_t persistence_revision;
    uint32_t persistence_commit_count;
    uint32_t persistence_fail_count;
    esp_err_t persistence_last_error;
    bool loaded;
    bool persistence_dirty;
    bool sd_primary_required;
    bool sd_primary_dirty;
    bool sd_primary_reconcile_pending;
    bool nvs_fallback_dirty;
    bool clear_tombstone_pending;
} d1l_read_state_stats_t;

esp_err_t d1l_read_state_init(void);
esp_err_t d1l_read_state_clear(void);
esp_err_t d1l_read_state_flush(void);
esp_err_t d1l_read_state_flush_if_due(void);
esp_err_t d1l_read_state_mark_public_read(void);
esp_err_t d1l_read_state_mark_dm_read(void);
esp_err_t d1l_read_state_mark_dm_thread_read(const char *fingerprint);
esp_err_t d1l_read_state_mark_all_read(void);
d1l_read_state_stats_t d1l_read_state_stats(void);
bool d1l_read_state_dm_entry_is_unread(const d1l_dm_entry_t *entry);
size_t d1l_read_state_copy_dm_threads(d1l_read_state_dm_thread_t *out_threads,
                                      size_t max_threads);
size_t d1l_read_state_copy_persisted_dm_cursors(
    d1l_read_state_persisted_dm_cursor_t *out_cursors, size_t max_cursors,
    uint32_t *out_accepted_sd_backend_generation);

#ifdef D1L_READ_STATE_TEST_HOOKS
void d1l_read_state_test_set_after_sd_read_hook(void (*hook)(void));
#endif
