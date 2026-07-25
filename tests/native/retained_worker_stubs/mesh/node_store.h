#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t persistence_commit_count;
    uint32_t persistence_fail_count;
    uint64_t persistence_revision;
    bool persistence_dirty;
    bool sd_primary_reconcile_pending;
} d1l_node_store_stats_t;

esp_err_t d1l_node_store_flush(void);
esp_err_t d1l_node_store_flush_if_due(void);
d1l_node_store_stats_t d1l_node_store_stats(void);
