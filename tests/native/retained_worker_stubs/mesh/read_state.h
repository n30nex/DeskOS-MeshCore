#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint64_t persistence_revision;
    uint32_t persistence_commit_count;
    uint32_t persistence_fail_count;
    bool persistence_dirty;
    bool sd_primary_reconcile_pending;
} d1l_read_state_stats_t;

esp_err_t d1l_read_state_flush(void);
esp_err_t d1l_read_state_flush_if_due(void);
d1l_read_state_stats_t d1l_read_state_stats(void);
