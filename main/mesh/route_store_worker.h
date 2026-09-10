#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "storage/retained_store_scheduler.h"

typedef struct {
    bool running;
    bool active_force;
    int64_t active_deadline_us;
    int64_t active_store_started_us;
    size_t active_store_index;
    char active_store_name[D1L_RETAINED_STORE_NAME_LEN];
    uint32_t pass_count;
    uint32_t forced_pass_count;
    uint32_t background_pass_count;
    uint32_t deadline_exhausted_count;
    uint32_t quiesce_cancelled_count;
    d1l_retained_store_pass_t last_pass;
} d1l_retained_store_worker_status_t;

esp_err_t d1l_route_store_worker_start(void);
/* The timeout is one absolute deadline for queue wait, flush-lock wait and all
 * six stores. Store callbacks are synchronous: a callback already in flight
 * may finish after the caller receives ESP_ERR_TIMEOUT, but no later store is
 * started. `d1l_retained_store_worker_status()` remains running with the
 * active store until that callback returns and the worker releases its lock. */
esp_err_t d1l_route_store_worker_force_flush(uint32_t timeout_ms);
/* Preempt and hold retained persistence for an urgent storage transition. */
esp_err_t d1l_route_store_worker_quiesce_begin(uint32_t timeout_ms);
/* Wait for another foreground owner, then preempt background persistence.
 * Call before taking a store lock; release with quiesce_end after the edit. */
esp_err_t d1l_route_store_worker_quiesce_for_edit(uint32_t timeout_ms);
/* Wait for the current retained persistence sequence without cancelling it. */
esp_err_t d1l_route_store_worker_quiesce_wait_begin(uint32_t timeout_ms);
void d1l_route_store_worker_quiesce_end(void);
/* True for non-owners once persistence preemption is requested or owned. */
bool d1l_route_store_persistence_should_yield(void);
/* Copy the last fully published common scheduler pass without observing a
 * partially updated descriptor sequence. */
void d1l_retained_store_worker_status(
    d1l_retained_store_worker_status_t *out_status);
