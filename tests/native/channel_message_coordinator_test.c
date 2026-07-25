#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mesh/channel_message_coordinator.h"
#include "mesh/channel_store.h"
#include "mesh/message_store.h"

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer)
{
    return buffer;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks_to_wait)
{
    (void)handle;
    (void)ticks_to_wait;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    (void)handle;
    return pdTRUE;
}

static d1l_message_entry_t s_rows[2];
static d1l_message_retained_snapshot_t s_snapshots[3];
static uint64_t s_observed_revisions[3];
static size_t s_snapshot_plan_count;
static size_t s_snapshot_calls;
static size_t s_stats_calls;
static size_t s_reconcile_calls;
static esp_err_t s_reconcile_error;

static void plan(const uint64_t *snapshots, const uint64_t *observed,
                 size_t count, esp_err_t reconcile_error)
{
    assert(snapshots && observed && count > 0U && count <= 3U);
    memset(s_snapshots, 0, sizeof(s_snapshots));
    memset(s_observed_revisions, 0, sizeof(s_observed_revisions));
    for (size_t i = 0U; i < count; ++i) {
        s_snapshots[i] = (d1l_message_retained_snapshot_t){
            .epoch = 7U,
            .next_seq = 12U,
            .clear_lineage = UINT64_C(0x12345678),
            .persistence_revision = snapshots[i],
        };
        s_observed_revisions[i] = observed[i];
    }
    s_snapshot_plan_count = count;
    s_snapshot_calls = 0U;
    s_stats_calls = 0U;
    s_reconcile_calls = 0U;
    s_reconcile_error = reconcile_error;
}

esp_err_t d1l_message_store_snapshot_retained(
    d1l_message_entry_t *out_entries, size_t max_entries, size_t *out_count,
    d1l_message_retained_snapshot_t *out_snapshot)
{
    assert(out_entries && max_entries >= 2U && out_count && out_snapshot);
    assert(s_snapshot_calls < s_snapshot_plan_count);
    out_entries[0] = s_rows[0];
    out_entries[1] = s_rows[1];
    *out_count = 2U;
    *out_snapshot = s_snapshots[s_snapshot_calls++];
    return ESP_OK;
}

d1l_message_store_stats_t d1l_message_store_stats(void)
{
    assert(s_snapshot_plan_count > 0U);
    const size_t index = s_stats_calls < s_snapshot_plan_count ?
        s_stats_calls : s_snapshot_plan_count - 1U;
    s_stats_calls++;
    return (d1l_message_store_stats_t){
        .persistence_revision = s_observed_revisions[index],
    };
}

esp_err_t d1l_channel_store_reconcile_retained_rows(
    const d1l_channel_retained_row_t *rows, size_t row_count,
    const d1l_channel_message_generation_t *message_generation)
{
    s_reconcile_calls++;
    assert(rows && message_generation);
#if EXPECT_CORE
    assert(row_count == 1U);
#else
    assert(row_count == 2U);
#endif
    assert(rows[0].channel_id == UINT64_C(1) &&
           rows[0].message_seq == 10U && rows[0].received);
#if !EXPECT_CORE
    assert(rows[1].channel_id == UINT64_C(2) &&
           rows[1].message_seq == 11U && !rows[1].received);
#endif
    assert(message_generation->epoch == 7U &&
           message_generation->next_seq == 12U &&
           message_generation->clear_lineage == UINT64_C(0x12345678));
    return s_reconcile_error;
}

int main(void)
{
    s_rows[0].channel_id = UINT64_C(1);
    s_rows[0].seq = 10U;
    memcpy(s_rows[0].direction, "rx", 3U);
    s_rows[1].channel_id = UINT64_C(2);
    s_rows[1].seq = 11U;
    memcpy(s_rows[1].direction, "tx", 3U);

    const uint64_t stable[] = {1U};
    plan(stable, stable, 1U, ESP_OK);
    assert(d1l_channel_message_reconcile() == ESP_OK);
    assert(s_reconcile_calls == 1U);
    assert(!d1l_channel_message_reconcile_pending());

    /* A hot-SD merge changes the message revision without an RF append. Owner
     * maintenance must notice and reconcile even when no retry was pending. */
    const uint64_t merged[] = {2U};
    plan(merged, merged, 1U, ESP_OK);
    assert(d1l_channel_message_reconcile_if_due(1U) == ESP_OK);
    assert(s_reconcile_calls == 1U);
    assert(!d1l_channel_message_reconcile_pending());

    const uint64_t retry_snapshot[] = {10U, 11U};
    const uint64_t retry_observed[] = {11U, 11U};
    plan(retry_snapshot, retry_observed, 2U, ESP_OK);
    assert(d1l_channel_message_reconcile() == ESP_OK);
    assert(s_reconcile_calls == 2U);
    assert(!d1l_channel_message_reconcile_pending());

    plan(stable, stable, 1U, ESP_FAIL);
    assert(d1l_channel_message_reconcile() == ESP_FAIL);
    assert(d1l_channel_message_reconcile_pending());
    plan(stable, stable, 1U, ESP_OK);
    assert(d1l_channel_message_reconcile_if_due(5001U) == ESP_OK);
    assert(s_reconcile_calls == 1U);
    assert(!d1l_channel_message_reconcile_pending());

    const uint64_t racing_snapshot[] = {20U, 21U, 22U};
    const uint64_t racing_observed[] = {21U, 22U, 23U};
    plan(racing_snapshot, racing_observed, 3U, ESP_OK);
    assert(d1l_channel_message_reconcile() == ESP_ERR_INVALID_STATE);
    assert(s_reconcile_calls == 3U);
    assert(d1l_channel_message_reconcile_pending());

    plan(stable, stable, 1U, ESP_OK);
    assert(d1l_channel_message_reconcile_if_due(10000U) == ESP_OK);
    assert(s_reconcile_calls == 0U);
    assert(d1l_channel_message_reconcile_pending());
    assert(d1l_channel_message_reconcile_if_due(10001U) == ESP_OK);
    assert(s_reconcile_calls == 1U);
    assert(!d1l_channel_message_reconcile_pending());

    puts("native channel-message coordinator: ok");
    return 0;
}
