#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define D1L_RETAINED_STORE_SCHEDULER_MAX_STORES 8U
#define D1L_RETAINED_STORE_NAME_LEN 16U

typedef enum {
    D1L_RETAINED_STORE_MESSAGES = 0,
    D1L_RETAINED_STORE_DIRECT_MESSAGES,
    D1L_RETAINED_STORE_PACKETS,
    D1L_RETAINED_STORE_ROUTES,
    D1L_RETAINED_STORE_CONTACTS,
    D1L_RETAINED_STORE_READ_STATE,
    D1L_RETAINED_STORE_TIME_CHECKPOINT,
    D1L_RETAINED_STORE_NODES,
} d1l_retained_store_kind_t;

typedef enum {
    D1L_RETAINED_STORE_OUTCOME_NOT_RUN = 0,
    D1L_RETAINED_STORE_OUTCOME_SKIPPED_CLEAN,
    D1L_RETAINED_STORE_OUTCOME_NO_CHANGE,
    D1L_RETAINED_STORE_OUTCOME_COMMITTED,
    D1L_RETAINED_STORE_OUTCOME_COALESCED,
    D1L_RETAINED_STORE_OUTCOME_FAILED,
    D1L_RETAINED_STORE_OUTCOME_CANCELLED_DEADLINE,
    D1L_RETAINED_STORE_OUTCOME_CANCELLED_QUIESCE,
} d1l_retained_store_outcome_t;

/* This is the common durability projection. Store-specific envelope and
 * serialization state remains private to each retained store. */
typedef struct {
    uint64_t revision;
    uint32_t commit_count;
    uint32_t failure_count;
    bool dirty;
    bool reconcile_pending;
} d1l_retained_store_observation_t;

typedef esp_err_t (*d1l_retained_store_flush_fn_t)(void *context);
typedef void (*d1l_retained_store_observe_fn_t)(
    void *context, d1l_retained_store_observation_t *out_observation);
typedef int64_t (*d1l_retained_store_clock_fn_t)(void *context);
typedef bool (*d1l_retained_store_cancel_fn_t)(void *context);
struct d1l_retained_store_result;
typedef void (*d1l_retained_store_progress_fn_t)(
    void *context, size_t store_index,
    const struct d1l_retained_store_result *result, bool starting);

typedef struct {
    d1l_retained_store_kind_t kind;
    const char *name;
    void *context;
    d1l_retained_store_flush_fn_t flush;
    d1l_retained_store_flush_fn_t flush_if_due;
    d1l_retained_store_observe_fn_t observe;
} d1l_retained_store_descriptor_t;

typedef struct {
    bool force;
    /* Required for forced passes and shared by every descriptor in the pass.
     * A value of zero means no deadline and is valid only for background work. */
    int64_t deadline_us;
    d1l_retained_store_clock_fn_t clock;
    void *clock_context;
    d1l_retained_store_cancel_fn_t cancel_requested;
    void *cancel_context;
    d1l_retained_store_progress_fn_t progress;
    void *progress_context;
} d1l_retained_store_scheduler_options_t;

typedef struct d1l_retained_store_result {
    d1l_retained_store_kind_t kind;
    char name[D1L_RETAINED_STORE_NAME_LEN];
    d1l_retained_store_outcome_t outcome;
    esp_err_t result;
    int64_t started_us;
    int64_t finished_us;
    d1l_retained_store_observation_t before;
    d1l_retained_store_observation_t after;
} d1l_retained_store_result_t;

typedef struct {
    bool force;
    bool deadline_exhausted;
    bool quiesce_cancelled;
    int64_t deadline_us;
    int64_t started_us;
    int64_t finished_us;
    esp_err_t result;
    size_t store_count;
    size_t attempted_count;
    size_t committed_count;
    size_t coalesced_count;
    size_t skipped_clean_count;
    size_t failed_count;
    d1l_retained_store_result_t stores[D1L_RETAINED_STORE_SCHEDULER_MAX_STORES];
} d1l_retained_store_pass_t;

/* Execute one descriptor pass. A forced pass is rejected unless it carries an
 * absolute deadline. Forced passes bypass due/backoff policy only for stores
 * observed dirty or reconciliation-pending; clean stores remain skipped.
 * No descriptor begins after that deadline, and quiesce or deadline
 * cancellation prevents all remaining descriptors from starting. */
esp_err_t d1l_retained_store_scheduler_run(
    const d1l_retained_store_descriptor_t *descriptors,
    size_t descriptor_count,
    const d1l_retained_store_scheduler_options_t *options,
    d1l_retained_store_pass_t *out_pass);

const char *d1l_retained_store_outcome_name(
    d1l_retained_store_outcome_t outcome);

/* Convert a microsecond wait to ceil-rounded scheduler ticks without overflow.
 * Positive waits are at least one tick and saturate at max_finite_ticks, which
 * callers must set strictly below their platform's infinite-wait sentinel. */
uint32_t d1l_retained_store_finite_wait_ticks(
    uint64_t remaining_us, uint32_t tick_rate_hz,
    uint32_t max_finite_ticks);
