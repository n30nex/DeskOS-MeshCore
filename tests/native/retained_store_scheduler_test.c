#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "storage/retained_store_scheduler.h"

typedef struct scheduler_fixture scheduler_fixture_t;

typedef struct {
    scheduler_fixture_t *fixture;
    d1l_retained_store_observation_t observation;
    esp_err_t flush_result;
    uint32_t flush_calls;
    uint32_t due_calls;
    uint32_t observe_calls;
    int64_t observe_advance_us;
    int64_t advance_us;
    bool commit_on_flush;
    bool request_cancel;
} fake_store_t;

struct scheduler_fixture {
    int64_t now_us;
    bool cancel;
    bool progress_active;
    uint32_t progress_starts;
    uint32_t progress_finishes;
    size_t progress_store_index;
    char progress_store_name[D1L_RETAINED_STORE_NAME_LEN];
    int64_t progress_advance_us;
    int64_t progress_finish_advance_us;
    size_t progress_finish_store_index;
    bool progress_request_cancel;
    fake_store_t stores[4];
    d1l_retained_store_descriptor_t descriptors[4];
};

static int64_t fake_clock(void *context)
{
    return ((scheduler_fixture_t *)context)->now_us;
}

static bool fake_cancel(void *context)
{
    return ((scheduler_fixture_t *)context)->cancel;
}

static void fake_progress(
    void *context, size_t store_index,
    const d1l_retained_store_result_t *result, bool starting)
{
    scheduler_fixture_t *fixture = context;
    fixture->progress_active = starting;
    fixture->progress_store_index = store_index;
    if (starting) {
        fixture->progress_starts++;
        strncpy(fixture->progress_store_name, result->name,
                sizeof(fixture->progress_store_name) - 1U);
        fixture->now_us += fixture->progress_advance_us;
        if (fixture->progress_request_cancel) {
            fixture->cancel = true;
        }
    } else {
        if (store_index == fixture->progress_finish_store_index) {
            fixture->now_us += fixture->progress_finish_advance_us;
        }
        fixture->progress_finishes++;
        fixture->progress_store_name[0] = '\0';
    }
}

static void fake_observe(
    void *context, d1l_retained_store_observation_t *out_observation)
{
    fake_store_t *store = context;
    store->observe_calls++;
    store->fixture->now_us += store->observe_advance_us;
    *out_observation = store->observation;
}

static esp_err_t fake_flush_common(fake_store_t *store, bool due)
{
    assert(store->fixture->progress_active);
    assert(store->fixture->progress_store_name[0] != '\0');
    if (due) {
        store->due_calls++;
    } else {
        store->flush_calls++;
    }
    store->fixture->now_us += store->advance_us;
    if (store->request_cancel) {
        store->fixture->cancel = true;
    }
    if (store->flush_result == ESP_OK && store->commit_on_flush) {
        store->observation.commit_count++;
        store->observation.dirty = false;
        store->observation.reconcile_pending = false;
    }
    if (store->flush_result != ESP_OK) {
        store->observation.failure_count++;
    }
    return store->flush_result;
}

static esp_err_t fake_flush(void *context)
{
    return fake_flush_common((fake_store_t *)context, false);
}

static esp_err_t fake_flush_if_due(void *context)
{
    return fake_flush_common((fake_store_t *)context, true);
}

static void fixture_init(scheduler_fixture_t *fixture)
{
    static const char *const names[] = {
        "messages", "direct_messages", "packets", "routes",
    };
    memset(fixture, 0, sizeof(*fixture));
    fixture->now_us = 1000;
    fixture->progress_finish_store_index = SIZE_MAX;
    for (size_t i = 0; i < 4U; ++i) {
        fixture->stores[i].fixture = fixture;
        fixture->stores[i].flush_result = ESP_OK;
        fixture->descriptors[i] = (d1l_retained_store_descriptor_t) {
            .kind = (d1l_retained_store_kind_t)i,
            .name = names[i],
            .context = &fixture->stores[i],
            .flush = fake_flush,
            .flush_if_due = fake_flush_if_due,
            .observe = fake_observe,
        };
    }
}

static d1l_retained_store_scheduler_options_t background_options(
    scheduler_fixture_t *fixture)
{
    return (d1l_retained_store_scheduler_options_t) {
        .clock = fake_clock,
        .clock_context = fixture,
        .cancel_requested = fake_cancel,
        .cancel_context = fixture,
        .progress = fake_progress,
        .progress_context = fixture,
    };
}

static d1l_retained_store_scheduler_options_t forced_options(
    scheduler_fixture_t *fixture, int64_t deadline_us)
{
    d1l_retained_store_scheduler_options_t options =
        background_options(fixture);
    for (size_t i = 0; i < 4U; ++i) {
        fixture->stores[i].observation.dirty = true;
    }
    options.force = true;
    options.deadline_us = deadline_us;
    return options;
}

static void test_forced_clean_pass_skips_all_stores(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    d1l_retained_store_scheduler_options_t options =
        forced_options(&fixture, fixture.now_us + 1000);
    for (size_t i = 0; i < 4U; ++i) {
        fixture.stores[i].observation.dirty = false;
    }

    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_OK);
    assert(pass.force);
    assert(!pass.deadline_exhausted);
    assert(pass.attempted_count == 0U);
    assert(pass.skipped_clean_count == 4U);
    assert(fixture.progress_starts == 0U);
    assert(fixture.progress_finishes == 0U);
    for (size_t i = 0; i < 4U; ++i) {
        assert(fixture.stores[i].flush_calls == 0U);
        assert(fixture.stores[i].due_calls == 0U);
        assert(pass.stores[i].outcome ==
               D1L_RETAINED_STORE_OUTCOME_SKIPPED_CLEAN);
    }
}

static void test_burst_mutations_coalesce_to_one_commit(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    fake_store_t *packets = &fixture.stores[2];
    for (uint32_t i = 0; i < 100U; ++i) {
        packets->observation.revision++;
        packets->observation.dirty = true;
    }
    packets->commit_on_flush = true;

    d1l_retained_store_pass_t pass = {0};
    d1l_retained_store_scheduler_options_t options =
        background_options(&fixture);
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_OK);
    assert(pass.attempted_count == 1U);
    assert(pass.committed_count == 1U);
    assert(pass.skipped_clean_count == 3U);
    assert(packets->due_calls == 1U);
    assert(packets->observation.commit_count == 1U);
    assert(!packets->observation.dirty);

    for (uint32_t i = 0; i < 100U; ++i) {
        assert(d1l_retained_store_scheduler_run(
                   fixture.descriptors, 4U, &options, &pass) == ESP_OK);
    }
    assert(packets->due_calls == 1U);
    assert(pass.attempted_count == 0U);
    assert(pass.skipped_clean_count == 4U);
}

static void test_due_work_reports_coalesced_state(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    fixture.stores[0].observation.dirty = true;

    d1l_retained_store_pass_t pass = {0};
    d1l_retained_store_scheduler_options_t options =
        background_options(&fixture);
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_OK);
    assert(pass.coalesced_count == 1U);
    assert(pass.stores[0].outcome ==
           D1L_RETAINED_STORE_OUTCOME_COALESCED);
    assert(pass.stores[0].before.dirty);
    assert(pass.stores[0].after.dirty);
    assert(strcmp(d1l_retained_store_outcome_name(
                      pass.stores[0].outcome),
                  "coalesced") == 0);
}

static void test_forced_deadline_stops_remaining_stores(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    fixture.stores[0].observation.dirty = true;
    fixture.stores[0].commit_on_flush = true;
    fixture.stores[0].advance_us = 40;
    d1l_retained_store_scheduler_options_t options =
        forced_options(&fixture, fixture.now_us + 30);

    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_ERR_TIMEOUT);
    assert(pass.deadline_exhausted);
    assert(pass.attempted_count == 1U);
    assert(fixture.stores[0].flush_calls == 1U);
    assert(fixture.stores[1].flush_calls == 0U);
    assert(pass.stores[0].outcome ==
           D1L_RETAINED_STORE_OUTCOME_COMMITTED);
    assert(!fixture.progress_active);
    assert(fixture.progress_starts == 1U);
    assert(fixture.progress_finishes == 1U);
    for (size_t i = 1U; i < 4U; ++i) {
        assert(pass.stores[i].outcome ==
               D1L_RETAINED_STORE_OUTCOME_CANCELLED_DEADLINE);
        assert(pass.stores[i].result == ESP_ERR_TIMEOUT);
    }

    /* The timed-out callback has returned, so a later forced request can run
     * every descriptor. This is the bounded recovery point for the worker's
     * flush lock; synchronous store I/O itself is not mid-call cancellable. */
    fixture.stores[0].advance_us = 0;
    fixture.stores[0].observation.dirty = true;
    options.deadline_us = fixture.now_us + 1000;
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_OK);
    assert(pass.attempted_count == 4U);
    assert(fixture.stores[1].flush_calls == 1U);
    assert(!fixture.progress_active);
}

static void test_expired_forced_pass_starts_no_store(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    d1l_retained_store_scheduler_options_t options =
        forced_options(&fixture, fixture.now_us);
    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_ERR_TIMEOUT);
    assert(pass.deadline_exhausted);
    assert(pass.attempted_count == 0U);
    for (size_t i = 0U; i < 4U; ++i) {
        assert(fixture.stores[i].flush_calls == 0U);
        assert(pass.stores[i].outcome ==
               D1L_RETAINED_STORE_OUTCOME_CANCELLED_DEADLINE);
    }
}

static void test_deadline_during_observation_starts_no_store(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    fixture.stores[0].observe_advance_us = 40;
    d1l_retained_store_scheduler_options_t options =
        forced_options(&fixture, fixture.now_us + 30);
    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_ERR_TIMEOUT);
    assert(pass.deadline_exhausted);
    assert(pass.attempted_count == 0U);
    assert(fixture.stores[0].observe_calls == 1U);
    assert(fixture.stores[0].flush_calls == 0U);
    assert(pass.stores[0].outcome ==
           D1L_RETAINED_STORE_OUTCOME_CANCELLED_DEADLINE);
}

static void test_deadline_result_precedes_inflight_store_failure(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    fixture.stores[0].flush_result = ESP_FAIL;
    fixture.stores[0].advance_us = 40;
    d1l_retained_store_scheduler_options_t options =
        forced_options(&fixture, fixture.now_us + 30);
    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_ERR_TIMEOUT);
    assert(pass.deadline_exhausted);
    assert(pass.failed_count == 1U);
    assert(pass.stores[0].result == ESP_FAIL);
    assert(pass.stores[0].outcome == D1L_RETAINED_STORE_OUTCOME_FAILED);
    assert(fixture.stores[1].flush_calls == 0U);
}

static void test_deadline_during_progress_starts_no_store(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    fixture.progress_advance_us = 40;
    d1l_retained_store_scheduler_options_t options =
        forced_options(&fixture, fixture.now_us + 30);
    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_ERR_TIMEOUT);
    assert(pass.deadline_exhausted);
    assert(pass.attempted_count == 0U);
    assert(fixture.progress_starts == 1U);
    assert(fixture.progress_finishes == 1U);
    assert(!fixture.progress_active);
    assert(fixture.stores[0].flush_calls == 0U);
    assert(pass.stores[0].outcome ==
           D1L_RETAINED_STORE_OUTCOME_CANCELLED_DEADLINE);
    assert(pass.stores[0].after.revision == pass.stores[0].before.revision);
}

static void test_quiesce_during_progress_starts_no_store(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    fixture.progress_request_cancel = true;
    d1l_retained_store_scheduler_options_t options =
        forced_options(&fixture, fixture.now_us + 1000);
    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_ERR_TIMEOUT);
    assert(pass.quiesce_cancelled);
    assert(!pass.deadline_exhausted);
    assert(pass.attempted_count == 0U);
    assert(fixture.progress_starts == 1U);
    assert(fixture.progress_finishes == 1U);
    assert(!fixture.progress_active);
    assert(fixture.stores[0].flush_calls == 0U);
    assert(pass.stores[0].outcome ==
           D1L_RETAINED_STORE_OUTCOME_CANCELLED_QUIESCE);
}

static void test_final_progress_finish_crosses_deadline(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    fixture.progress_finish_store_index = 3U;
    fixture.progress_finish_advance_us = 40;
    fixture.stores[3].observation.dirty = true;
    fixture.stores[3].commit_on_flush = true;
    d1l_retained_store_scheduler_options_t options =
        forced_options(&fixture, fixture.now_us + 30);
    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_ERR_TIMEOUT);
    assert(pass.deadline_exhausted);
    assert(pass.attempted_count == 4U);
    assert(pass.stores[3].result == ESP_OK);
    assert(pass.stores[3].outcome ==
           D1L_RETAINED_STORE_OUTCOME_COMMITTED);
    assert(pass.committed_count == 1U);
    assert(pass.finished_us >= options.deadline_us);
    assert(!fixture.progress_active);
}

static void test_quiesce_cancels_force_after_current_store(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    fixture.stores[0].request_cancel = true;
    d1l_retained_store_scheduler_options_t options =
        forced_options(&fixture, fixture.now_us + 1000);
    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_ERR_TIMEOUT);
    assert(pass.quiesce_cancelled);
    assert(pass.attempted_count == 1U);
    assert(fixture.stores[0].flush_calls == 1U);
    assert(fixture.stores[1].flush_calls == 0U);
    assert(pass.stores[1].outcome ==
           D1L_RETAINED_STORE_OUTCOME_CANCELLED_QUIESCE);
}

static void test_first_failure_is_retained_while_other_stores_run(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    fixture.stores[0].flush_result = ESP_FAIL;
    fixture.stores[1].commit_on_flush = true;
    fixture.stores[1].observation.dirty = true;
    d1l_retained_store_scheduler_options_t options =
        forced_options(&fixture, fixture.now_us + 1000);
    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_FAIL);
    assert(pass.failed_count == 1U);
    assert(pass.stores[0].after.failure_count == 1U);
    assert(pass.stores[0].outcome == D1L_RETAINED_STORE_OUTCOME_FAILED);
    assert(fixture.stores[3].flush_calls == 1U);
}

static void test_forced_pass_requires_shared_deadline(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    d1l_retained_store_scheduler_options_t options =
        forced_options(&fixture, 0);
    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) ==
           ESP_ERR_INVALID_ARG);
}

static void test_background_quiesce_cancellation_is_clean(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    fixture.cancel = true;
    d1l_retained_store_scheduler_options_t options =
        background_options(&fixture);
    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_OK);
    assert(pass.quiesce_cancelled);
    assert(pass.attempted_count == 0U);
    for (size_t i = 0U; i < 4U; ++i) {
        assert(pass.stores[i].outcome ==
               D1L_RETAINED_STORE_OUTCOME_CANCELLED_QUIESCE);
        assert(pass.stores[i].result == ESP_OK);
    }
}

static void test_descriptor_name_is_nul_terminated(void)
{
    scheduler_fixture_t fixture;
    fixture_init(&fixture);
    fixture.descriptors[0].name = "messages-name-longer-than-buffer";
    d1l_retained_store_scheduler_options_t options =
        background_options(&fixture);
    d1l_retained_store_pass_t pass = {0};
    assert(d1l_retained_store_scheduler_run(
               fixture.descriptors, 4U, &options, &pass) == ESP_OK);
    assert(pass.stores[0].name[D1L_RETAINED_STORE_NAME_LEN - 1U] == '\0');
    assert(strlen(pass.stores[0].name) ==
           D1L_RETAINED_STORE_NAME_LEN - 1U);
}

static void test_finite_tick_conversion_boundaries(void)
{
    const uint32_t max_finite = UINT32_MAX - 1U;
    assert(d1l_retained_store_finite_wait_ticks(
               0U, 1000U, max_finite) == 0U);
    assert(d1l_retained_store_finite_wait_ticks(
               1U, 1000U, max_finite) == 1U);
    assert(d1l_retained_store_finite_wait_ticks(
               999U, 1000U, max_finite) == 1U);
    assert(d1l_retained_store_finite_wait_ticks(
               1000U, 1000U, max_finite) == 1U);
    assert(d1l_retained_store_finite_wait_ticks(
               1001U, 1000U, max_finite) == 2U);
    assert(d1l_retained_store_finite_wait_ticks(
               ((uint64_t)max_finite - 1ULL) * 1000ULL,
               1000U, max_finite) == max_finite - 1U);
    assert(d1l_retained_store_finite_wait_ticks(
               ((uint64_t)max_finite - 1ULL) * 1000ULL + 1ULL,
               1000U, max_finite) == max_finite);
    assert(d1l_retained_store_finite_wait_ticks(
               UINT64_MAX, 1000U, max_finite) == max_finite);
    assert(d1l_retained_store_finite_wait_ticks(
               (uint64_t)UINT32_MAX * 1000ULL,
               1000U, max_finite) == max_finite);
    assert(d1l_retained_store_finite_wait_ticks(
               1U, 0U, max_finite) == 0U);
    assert(d1l_retained_store_finite_wait_ticks(
               1U, 1000U, 0U) == 0U);
}

int main(void)
{
    test_forced_clean_pass_skips_all_stores();
    test_burst_mutations_coalesce_to_one_commit();
    test_due_work_reports_coalesced_state();
    test_forced_deadline_stops_remaining_stores();
    test_expired_forced_pass_starts_no_store();
    test_deadline_during_observation_starts_no_store();
    test_deadline_result_precedes_inflight_store_failure();
    test_deadline_during_progress_starts_no_store();
    test_quiesce_during_progress_starts_no_store();
    test_final_progress_finish_crosses_deadline();
    test_quiesce_cancels_force_after_current_store();
    test_first_failure_is_retained_while_other_stores_run();
    test_forced_pass_requires_shared_deadline();
    test_background_quiesce_cancellation_is_clean();
    test_descriptor_name_is_nul_terminated();
    test_finite_tick_conversion_boundaries();
    puts("native retained-store scheduler: ok");
    return 0;
}
