#pragma once

#include "esp_err.h"
#include "storage/retained_store_scheduler.h"

esp_err_t d1l_draft_store_flush(void);
esp_err_t d1l_draft_store_flush_if_due(void);
void d1l_draft_store_observe(d1l_retained_store_observation_t *out);
