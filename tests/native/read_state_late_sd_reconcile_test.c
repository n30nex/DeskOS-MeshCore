#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#include "mesh/read_state.h"
#include "mock_esp_nvs.h"
#include "storage/retained_blob_store.h"

#define READ_STATE_NAMESPACE "d1l_read"
#define READ_STATE_KEY "state"
#define READ_STATE_STORE D1L_RETAINED_BLOB_STORE_READ_STATE

typedef struct {
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    uint32_t last_read_seq;
} test_cursor_t;

typedef struct {
    uint32_t schema;
    uint32_t last_public_read_seq;
    uint32_t last_dm_read_seq;
    uint32_t mark_read_count;
} test_v1_blob_t;

typedef struct {
    uint32_t schema;
    uint32_t last_public_read_seq;
    uint32_t last_dm_read_seq;
    uint32_t mark_read_count;
    uint32_t dm_cursor_count;
    test_cursor_t dm_cursors[D1L_READ_STATE_DM_THREAD_CAPACITY];
} test_v2_blob_t;

void d1l_test_retained_blob_store_reset(void);
void d1l_test_retained_blob_store_set_backend(
    d1l_retained_blob_store_id_t store_id, bool enabled,
    uint32_t generation);
bool d1l_test_retained_blob_store_seed_sd(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len);
size_t d1l_test_retained_blob_store_copy_sd(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t dst_size);
size_t d1l_test_retained_blob_store_sd_read_call_count(
    d1l_retained_blob_store_id_t store_id);
size_t d1l_test_retained_blob_store_sd_write_commit_count(
    d1l_retained_blob_store_id_t store_id);
size_t d1l_test_retained_blob_store_sd_erase_commit_count(
    d1l_retained_blob_store_id_t store_id);
void d1l_test_retained_blob_store_change_after_next_sd_read(
    d1l_retained_blob_store_id_t store_id, bool enabled,
    uint32_t generation);
void d1l_test_retained_blob_store_change_before_next_sd_write(
    d1l_retained_blob_store_id_t store_id, bool enabled,
    uint32_t generation);
void d1l_test_retained_blob_store_change_before_next_sd_erase(
    d1l_retained_blob_store_id_t store_id, bool enabled,
    uint32_t generation);

static bool s_competing_clear_started;
static bool s_competing_clear_finished;
static esp_err_t s_competing_clear_result;
#ifdef _WIN32
static HANDLE s_competing_clear_thread;

static DWORD WINAPI run_competing_clear(void *unused)
#else
static pthread_t s_competing_clear_thread;

static void *run_competing_clear(void *unused)
#endif
{
    (void)unused;
    __atomic_store_n(&s_competing_clear_started, true, __ATOMIC_RELEASE);
    s_competing_clear_result = d1l_read_state_clear();
    __atomic_store_n(&s_competing_clear_finished, true, __ATOMIC_RELEASE);
#ifdef _WIN32
    return 0U;
#else
    return NULL;
#endif
}

static void start_competing_clear_after_sd_read(void)
{
#ifdef _WIN32
    s_competing_clear_thread = CreateThread(
        NULL, 0U, run_competing_clear, NULL, 0U, NULL);
    assert(s_competing_clear_thread != NULL);
#else
    assert(pthread_create(
               &s_competing_clear_thread, NULL,
               run_competing_clear, NULL) == 0);
#endif
    while (!__atomic_load_n(
        &s_competing_clear_started, __ATOMIC_ACQUIRE)) {
#ifdef _WIN32
        (void)SwitchToThread();
#else
        (void)sched_yield();
#endif
    }
    for (size_t i = 0U; i < 1000U; ++i) {
#ifdef _WIN32
        (void)SwitchToThread();
#else
        (void)sched_yield();
#endif
    }
    /* The flush owns the persistence/I/O lock while this hook runs. */
    assert(!__atomic_load_n(
        &s_competing_clear_finished, __ATOMIC_ACQUIRE));
}

static void join_competing_clear(void)
{
#ifdef _WIN32
    assert(WaitForSingleObject(
               s_competing_clear_thread, INFINITE) == WAIT_OBJECT_0);
    assert(CloseHandle(s_competing_clear_thread));
    s_competing_clear_thread = NULL;
#else
    assert(pthread_join(s_competing_clear_thread, NULL) == 0);
#endif
}

d1l_message_store_stats_t d1l_message_store_stats(void)
{
    return (d1l_message_store_stats_t) {.next_seq = UINT32_MAX};
}

size_t d1l_message_store_copy_recent(d1l_message_entry_t *out_entries,
                                     size_t max_entries)
{
    (void)out_entries;
    (void)max_entries;
    return 0U;
}

d1l_dm_store_stats_t d1l_dm_store_stats(void)
{
    return (d1l_dm_store_stats_t) {.next_seq = UINT32_MAX};
}

size_t d1l_dm_store_copy_recent(d1l_dm_entry_t *out_entries,
                                size_t max_entries)
{
    (void)out_entries;
    (void)max_entries;
    return 0U;
}

bool d1l_contact_store_find_by_fingerprint(const char *fingerprint,
                                           d1l_contact_entry_t *out_entry)
{
    (void)fingerprint;
    (void)out_entry;
    return false;
}

static test_v2_blob_t v2_blob(uint32_t public_seq, uint32_t dm_seq,
                              uint32_t mark_count)
{
    return (test_v2_blob_t) {
        .schema = 2U,
        .last_public_read_seq = public_seq,
        .last_dm_read_seq = dm_seq,
        .mark_read_count = mark_count,
    };
}

static void add_cursor(test_v2_blob_t *blob, const char *fingerprint,
                       uint32_t last_read_seq)
{
    assert(blob && fingerprint);
    assert(blob->dm_cursor_count < D1L_READ_STATE_DM_THREAD_CAPACITY);
    test_cursor_t *cursor = &blob->dm_cursors[blob->dm_cursor_count++];
    snprintf(cursor->fingerprint, sizeof(cursor->fingerprint), "%s",
             fingerprint);
    cursor->last_read_seq = last_read_seq;
}

static uint32_t cursor_seq(const test_v2_blob_t *blob,
                           const char *fingerprint)
{
    assert(blob && fingerprint);
    for (uint32_t i = 0U; i < blob->dm_cursor_count; ++i) {
        if (strncmp(blob->dm_cursors[i].fingerprint, fingerprint,
                    D1L_NODE_FINGERPRINT_LEN) == 0) {
            return blob->dm_cursors[i].last_read_seq;
        }
    }
    return 0U;
}

static void reset_case(bool enabled, uint32_t generation)
{
    mock_nvs_reset();
    d1l_test_retained_blob_store_reset();
    d1l_test_retained_blob_store_set_backend(
        READ_STATE_STORE, enabled, generation);
}

static test_v2_blob_t copy_sd_blob(void)
{
    test_v2_blob_t blob = {0};
    assert(d1l_test_retained_blob_store_copy_sd(
               READ_STATE_STORE, READ_STATE_KEY,
               &blob, sizeof(blob)) == sizeof(blob));
    return blob;
}

static void test_clean_late_sd_adopt_and_stats_projection(void)
{
    reset_case(false, 1U);
    assert(d1l_read_state_init() == ESP_OK);
    d1l_read_state_stats_t stats = d1l_read_state_stats();
    assert(stats.loaded);
    assert(stats.accepted_sd_backend_generation == 1U);
    assert(!stats.persistence_dirty);

    test_v2_blob_t sd = v2_blob(7U, 6U, 2U);
    add_cursor(&sd, "aaaaaaaaaaaaaaaa", 5U);
    assert(d1l_test_retained_blob_store_seed_sd(
        READ_STATE_STORE, READ_STATE_KEY, &sd, sizeof(sd)));
    d1l_test_retained_blob_store_set_backend(
        READ_STATE_STORE, true, 2U);

    const size_t reads_before =
        d1l_test_retained_blob_store_sd_read_call_count(READ_STATE_STORE);
    const size_t nvs_sets_before = mock_nvs_set_call_count();
    const size_t nvs_erases_before = mock_nvs_erase_call_count();
    const uint64_t revision_before =
        d1l_read_state_stats().persistence_revision;
    stats = d1l_read_state_stats();
    assert(stats.accepted_sd_backend_generation == 1U);
    assert(stats.sd_backend_generation == 2U);
    assert(stats.sd_primary_required);
    assert(stats.sd_primary_reconcile_pending);
    assert(stats.persistence_dirty);
    assert(stats.persistence_revision == revision_before);
    assert(d1l_test_retained_blob_store_sd_read_call_count(
               READ_STATE_STORE) == reads_before);
    assert(mock_nvs_set_call_count() == nvs_sets_before);
    assert(mock_nvs_erase_call_count() == nvs_erases_before);

    assert(d1l_read_state_flush_if_due() == ESP_OK);
    stats = d1l_read_state_stats();
    assert(stats.accepted_sd_backend_generation == 2U);
    assert(stats.last_public_read_seq == 7U);
    assert(stats.last_dm_read_seq == 6U);
    assert(stats.mark_read_count == 2U);
    assert(stats.persistence_revision == revision_before + 1U);
    assert(stats.persistence_commit_count == 0U);
    assert(!stats.persistence_dirty);
    assert(!stats.sd_primary_reconcile_pending);
    assert(d1l_test_retained_blob_store_sd_write_commit_count(
               READ_STATE_STORE) == 0U);
}

static void test_v1_sd_decode(void)
{
    reset_case(true, 5U);
    const test_v1_blob_t legacy = {
        .schema = 1U,
        .last_public_read_seq = 13U,
        .last_dm_read_seq = 11U,
        .mark_read_count = 4U,
    };
    assert(d1l_test_retained_blob_store_seed_sd(
        READ_STATE_STORE, READ_STATE_KEY, &legacy, sizeof(legacy)));
    assert(d1l_read_state_init() == ESP_OK);
    const d1l_read_state_stats_t stats = d1l_read_state_stats();
    assert(stats.accepted_sd_backend_generation == 5U);
    assert(stats.last_public_read_seq == 13U);
    assert(stats.last_dm_read_seq == 11U);
    assert(stats.mark_read_count == 4U);
    assert(d1l_test_retained_blob_store_sd_write_commit_count(
               READ_STATE_STORE) == 0U);
}

static void test_monotonic_merge_and_fallback_retirement(void)
{
    reset_case(false, 10U);
    test_v2_blob_t local = v2_blob(9U, 4U, 3U);
    add_cursor(&local, "aaaaaaaaaaaaaaaa", 8U);
    assert(mock_nvs_seed_blob(
        READ_STATE_NAMESPACE, READ_STATE_KEY, &local, sizeof(local)));
    assert(d1l_read_state_init() == ESP_OK);

    test_v2_blob_t sd = v2_blob(6U, 12U, 5U);
    add_cursor(&sd, "aaaaaaaaaaaaaaaa", 7U);
    add_cursor(&sd, "bbbbbbbbbbbbbbbb", 10U);
    assert(d1l_test_retained_blob_store_seed_sd(
        READ_STATE_STORE, READ_STATE_KEY, &sd, sizeof(sd)));
    d1l_test_retained_blob_store_set_backend(
        READ_STATE_STORE, true, 11U);

    d1l_read_state_stats_t stats = d1l_read_state_stats();
    assert(stats.accepted_sd_backend_generation == 10U);
    assert(stats.sd_primary_reconcile_pending);
    assert(d1l_read_state_flush() == ESP_OK);

    const test_v2_blob_t merged = copy_sd_blob();
    assert(merged.last_public_read_seq == 9U);
    assert(merged.last_dm_read_seq == 12U);
    assert(merged.mark_read_count == 5U);
    assert(cursor_seq(&merged, "aaaaaaaaaaaaaaaa") == 8U);
    assert(cursor_seq(&merged, "bbbbbbbbbbbbbbbb") == 10U);
    assert(mock_nvs_copy_blob(
               READ_STATE_NAMESPACE, READ_STATE_KEY, NULL, 0U) == 0U);
    stats = d1l_read_state_stats();
    assert(stats.accepted_sd_backend_generation == 11U);
    assert(stats.persisted_dm_cursor_count == 2U);
    assert(stats.persisted_dm_cursor_capacity ==
           D1L_READ_STATE_DM_THREAD_CAPACITY);
    assert(stats.persistence_revision == 1U);
    assert(stats.persistence_commit_count == 1U);
    assert(stats.persistence_fail_count == 0U);
    assert(stats.persistence_last_error == ESP_OK);
    d1l_read_state_persisted_dm_cursor_t persisted[2U] = {0};
    uint32_t cursor_generation = 0U;
    assert(d1l_read_state_copy_persisted_dm_cursors(
               persisted, 1U, &cursor_generation) == 1U);
    assert(cursor_generation == 11U);
    assert(d1l_read_state_copy_persisted_dm_cursors(
               persisted, 2U, &cursor_generation) == 2U);
    assert(cursor_generation == 11U);
    assert(strncmp(persisted[0].fingerprint, "aaaaaaaaaaaaaaaa",
                   D1L_NODE_FINGERPRINT_LEN) == 0);
    assert(persisted[0].last_read_seq == 8U);
    assert(strncmp(persisted[1].fingerprint, "bbbbbbbbbbbbbbbb",
                   D1L_NODE_FINGERPRINT_LEN) == 0);
    assert(persisted[1].last_read_seq == 10U);

    /* The retired local journal must not overwrite a later fresh SD
     * generation. A clean store adopts that generation as-is. */
    d1l_test_retained_blob_store_set_backend(
        READ_STATE_STORE, false, 12U);
    test_v2_blob_t fresh = v2_blob(2U, 1U, 1U);
    assert(d1l_test_retained_blob_store_seed_sd(
        READ_STATE_STORE, READ_STATE_KEY, &fresh, sizeof(fresh)));
    const size_t writes_before_fresh_adopt =
        d1l_test_retained_blob_store_sd_write_commit_count(
            READ_STATE_STORE);
    d1l_test_retained_blob_store_set_backend(
        READ_STATE_STORE, true, 13U);
    assert(d1l_read_state_flush() == ESP_OK);
    stats = d1l_read_state_stats();
    assert(stats.accepted_sd_backend_generation == 13U);
    assert(stats.last_public_read_seq == 2U);
    assert(stats.last_dm_read_seq == 1U);
    assert(d1l_test_retained_blob_store_sd_write_commit_count(
               READ_STATE_STORE) == writes_before_fresh_adopt);
}

static void test_clear_tombstone_survives_reboot_then_retires(void)
{
    reset_case(false, 20U);
    assert(d1l_read_state_init() == ESP_OK);
    assert(d1l_read_state_clear() == ESP_OK);
    test_v2_blob_t tombstone = {0};
    assert(mock_nvs_copy_blob(
               READ_STATE_NAMESPACE, READ_STATE_KEY,
               &tombstone, sizeof(tombstone)) == sizeof(tombstone));
    assert(tombstone.schema == 2U);
    assert(tombstone.last_public_read_seq == 0U);
    assert(tombstone.dm_cursor_count == 0U);
    d1l_read_state_stats_t stats = d1l_read_state_stats();
    assert(stats.clear_tombstone_pending);
    assert(stats.persistence_revision == 1U);

    /* d1l_read_state_init() models a reboot while the SD backend remains
     * disabled; the empty persisted v2 blob retains explicit-clear authority. */
    assert(d1l_read_state_init() == ESP_OK);
    stats = d1l_read_state_stats();
    assert(stats.clear_tombstone_pending);
    assert(stats.accepted_sd_backend_generation == 20U);

    test_v2_blob_t stale_sd = v2_blob(99U, 88U, 7U);
    add_cursor(&stale_sd, "aaaaaaaaaaaaaaaa", 77U);
    assert(d1l_test_retained_blob_store_seed_sd(
        READ_STATE_STORE, READ_STATE_KEY, &stale_sd, sizeof(stale_sd)));
    d1l_test_retained_blob_store_set_backend(
        READ_STATE_STORE, true, 21U);
    assert(d1l_read_state_flush() == ESP_OK);
    assert(d1l_test_retained_blob_store_copy_sd(
               READ_STATE_STORE, READ_STATE_KEY, NULL, 0U) == 0U);
    assert(mock_nvs_copy_blob(
               READ_STATE_NAMESPACE, READ_STATE_KEY, NULL, 0U) == 0U);
    assert(d1l_test_retained_blob_store_sd_erase_commit_count(
               READ_STATE_STORE) == 1U);
    stats = d1l_read_state_stats();
    assert(stats.accepted_sd_backend_generation == 21U);
    assert(stats.persistence_commit_count == 1U);
    assert(!stats.clear_tombstone_pending);
    assert(!stats.persistence_dirty);

    assert(d1l_read_state_init() == ESP_OK);
    stats = d1l_read_state_stats();
    assert(stats.loaded);
    assert(!stats.clear_tombstone_pending);
    assert(stats.last_public_read_seq == 0U);
}

static void test_generation_races_do_not_apply_or_write_replacement(void)
{
    reset_case(false, 30U);
    assert(d1l_read_state_init() == ESP_OK);
    test_v2_blob_t remote = v2_blob(5U, 4U, 2U);
    assert(d1l_test_retained_blob_store_seed_sd(
        READ_STATE_STORE, READ_STATE_KEY, &remote, sizeof(remote)));
    d1l_test_retained_blob_store_set_backend(
        READ_STATE_STORE, true, 31U);
    d1l_test_retained_blob_store_change_after_next_sd_read(
        READ_STATE_STORE, true, 32U);
    assert(d1l_read_state_flush() == ESP_ERR_INVALID_STATE);
    d1l_read_state_stats_t stats = d1l_read_state_stats();
    assert(stats.accepted_sd_backend_generation == 30U);
    assert(stats.sd_backend_generation == 32U);
    assert(stats.last_public_read_seq == 0U);
    assert(stats.persistence_revision == 0U);
    assert(stats.persistence_fail_count == 1U);
    assert(stats.sd_primary_reconcile_pending);
    assert(d1l_test_retained_blob_store_sd_write_commit_count(
               READ_STATE_STORE) == 0U);
    assert(d1l_read_state_flush() == ESP_OK);
    stats = d1l_read_state_stats();
    assert(stats.accepted_sd_backend_generation == 32U);
    assert(stats.last_public_read_seq == 5U);
    assert(stats.persistence_revision == 1U);
    assert(stats.persistence_last_error == ESP_OK);

    reset_case(false, 40U);
    test_v2_blob_t local = v2_blob(9U, 3U, 2U);
    add_cursor(&local, "aaaaaaaaaaaaaaaa", 8U);
    assert(mock_nvs_seed_blob(
        READ_STATE_NAMESPACE, READ_STATE_KEY, &local, sizeof(local)));
    assert(d1l_read_state_init() == ESP_OK);
    remote = v2_blob(4U, 10U, 4U);
    add_cursor(&remote, "aaaaaaaaaaaaaaaa", 6U);
    assert(d1l_test_retained_blob_store_seed_sd(
        READ_STATE_STORE, READ_STATE_KEY, &remote, sizeof(remote)));
    d1l_test_retained_blob_store_set_backend(
        READ_STATE_STORE, true, 41U);
    d1l_test_retained_blob_store_change_before_next_sd_write(
        READ_STATE_STORE, true, 42U);
    assert(d1l_read_state_flush() == ESP_ERR_INVALID_STATE);
    test_v2_blob_t unchanged = copy_sd_blob();
    assert(unchanged.last_public_read_seq == 4U);
    assert(unchanged.last_dm_read_seq == 10U);
    assert(d1l_test_retained_blob_store_sd_write_commit_count(
               READ_STATE_STORE) == 0U);
    stats = d1l_read_state_stats();
    assert(stats.accepted_sd_backend_generation == 40U);
    assert(stats.sd_backend_generation == 42U);
    assert(stats.last_public_read_seq == 9U);
    assert(stats.last_dm_read_seq == 3U);
    assert(stats.persistence_revision == 0U);
    assert(stats.persistence_fail_count == 1U);
    assert(mock_nvs_copy_blob(
               READ_STATE_NAMESPACE, READ_STATE_KEY, NULL, 0U) ==
           sizeof(local));
    assert(d1l_read_state_flush() == ESP_OK);
    const test_v2_blob_t retried = copy_sd_blob();
    assert(retried.last_public_read_seq == 9U);
    assert(retried.last_dm_read_seq == 10U);
    assert(cursor_seq(&retried, "aaaaaaaaaaaaaaaa") == 8U);
    assert(mock_nvs_copy_blob(
               READ_STATE_NAMESPACE, READ_STATE_KEY, NULL, 0U) == 0U);

    reset_case(false, 50U);
    assert(d1l_read_state_init() == ESP_OK);
    assert(d1l_read_state_clear() == ESP_OK);
    remote = v2_blob(100U, 90U, 8U);
    assert(d1l_test_retained_blob_store_seed_sd(
        READ_STATE_STORE, READ_STATE_KEY, &remote, sizeof(remote)));
    d1l_test_retained_blob_store_set_backend(
        READ_STATE_STORE, true, 51U);
    d1l_test_retained_blob_store_change_before_next_sd_erase(
        READ_STATE_STORE, true, 52U);
    assert(d1l_read_state_flush() == ESP_ERR_INVALID_STATE);
    unchanged = copy_sd_blob();
    assert(unchanged.last_public_read_seq == 100U);
    assert(d1l_test_retained_blob_store_sd_erase_commit_count(
               READ_STATE_STORE) == 0U);
    stats = d1l_read_state_stats();
    assert(stats.clear_tombstone_pending);
    assert(stats.accepted_sd_backend_generation == 50U);
    assert(stats.sd_backend_generation == 52U);
    assert(d1l_read_state_flush() == ESP_OK);
    assert(d1l_test_retained_blob_store_copy_sd(
               READ_STATE_STORE, READ_STATE_KEY, NULL, 0U) == 0U);
    assert(mock_nvs_copy_blob(
               READ_STATE_NAMESPACE, READ_STATE_KEY, NULL, 0U) == 0U);
}

static void test_flush_and_clear_are_serialized(void)
{
    reset_case(false, 60U);
    assert(d1l_read_state_init() == ESP_OK);
    test_v2_blob_t remote = v2_blob(17U, 15U, 4U);
    add_cursor(&remote, "aaaaaaaaaaaaaaaa", 12U);
    assert(d1l_test_retained_blob_store_seed_sd(
        READ_STATE_STORE, READ_STATE_KEY, &remote, sizeof(remote)));
    d1l_test_retained_blob_store_set_backend(
        READ_STATE_STORE, true, 61U);

    s_competing_clear_started = false;
    s_competing_clear_finished = false;
    s_competing_clear_result = ESP_FAIL;
    d1l_read_state_test_set_after_sd_read_hook(
        start_competing_clear_after_sd_read);
    assert(d1l_read_state_flush() == ESP_OK);
    join_competing_clear();
    assert(s_competing_clear_result == ESP_OK);
    assert(__atomic_load_n(
        &s_competing_clear_finished, __ATOMIC_ACQUIRE));

    /* A stale flush replacement must never restore the SD rows after the
     * queued clear acquires the locks. */
    assert(d1l_test_retained_blob_store_copy_sd(
               READ_STATE_STORE, READ_STATE_KEY, NULL, 0U) == 0U);
    assert(mock_nvs_copy_blob(
               READ_STATE_NAMESPACE, READ_STATE_KEY, NULL, 0U) == 0U);
    const d1l_read_state_stats_t stats = d1l_read_state_stats();
    assert(stats.accepted_sd_backend_generation == 61U);
    assert(stats.last_public_read_seq == 0U);
    assert(stats.last_dm_read_seq == 0U);
    assert(stats.persistence_revision == 2U);
    assert(stats.persistence_commit_count == 1U);
    assert(!stats.persistence_dirty);
}

int main(void)
{
    test_clean_late_sd_adopt_and_stats_projection();
    test_v1_sd_decode();
    test_monotonic_merge_and_fallback_retirement();
    test_clear_tombstone_survives_reboot_then_retires();
    test_generation_races_do_not_apply_or_write_replacement();
    test_flush_and_clear_are_serialized();
    puts("native read-state late SD reconciliation: ok");
    return 0;
}
