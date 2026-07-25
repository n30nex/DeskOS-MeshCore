#include "channel_message_coordinator.h"

#include <stddef.h>

#include "app/release_profile.h"
#include "esp_attr.h"
#include "mesh/channel_store.h"
#include "mesh/message_store.h"
#include "mesh/store_lock.h"

#define D1L_CHANNEL_MESSAGE_RECONCILE_ATTEMPTS 3U

_Static_assert(D1L_CHANNEL_RETAINED_ROW_CAPACITY ==
                   D1L_MESSAGE_STORE_CAPACITY,
               "channel reconciliation must cover the complete message ring");

static d1l_message_entry_t
    s_message_rows[D1L_MESSAGE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static d1l_channel_retained_row_t
    s_channel_rows[D1L_CHANNEL_RETAINED_ROW_CAPACITY] EXT_RAM_BSS_ATTR;
static d1l_store_lock_t s_reconcile_lock = D1L_STORE_LOCK_INITIALIZER;
static bool s_reconcile_pending;
static uint32_t s_last_retry_ms;
static uint64_t s_last_reconciled_message_revision;

esp_err_t d1l_channel_message_reconcile(void)
{
    d1l_store_lock_take(&s_reconcile_lock);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    for (size_t attempt = 0U;
        attempt < D1L_CHANNEL_MESSAGE_RECONCILE_ATTEMPTS; ++attempt) {
        size_t row_count = 0U;
        d1l_message_retained_snapshot_t message_snapshot = {0};
        ret = d1l_message_store_snapshot_retained(
            s_message_rows, D1L_MESSAGE_STORE_CAPACITY, &row_count,
            &message_snapshot);
        if (ret != ESP_OK) {
            break;
        }
        size_t admitted_row_count = 0U;
        for (size_t i = 0U; i < row_count; ++i) {
            if (d1l_release_profile_is_core() &&
                s_message_rows[i].channel_id != D1L_CHANNEL_PUBLIC_ID) {
                continue;
            }
            s_channel_rows[admitted_row_count++] =
                (d1l_channel_retained_row_t){
                .channel_id = s_message_rows[i].channel_id,
                .message_seq = s_message_rows[i].seq,
                .received = s_message_rows[i].direction[0] == 'r',
            };
        }
        const d1l_channel_message_generation_t message_generation = {
            .epoch = message_snapshot.epoch,
            .next_seq = message_snapshot.next_seq,
            .clear_lineage = message_snapshot.clear_lineage,
        };
        ret = d1l_channel_store_reconcile_retained_rows(
            s_channel_rows, admitted_row_count, &message_generation);
        if (ret != ESP_OK) {
            break;
        }
        if (d1l_message_store_stats().persistence_revision ==
            message_snapshot.persistence_revision) {
            s_reconcile_pending = false;
            s_last_reconciled_message_revision =
                message_snapshot.persistence_revision;
            d1l_store_lock_give(&s_reconcile_lock);
            return ESP_OK;
        }
    }
    s_reconcile_pending = true;
    d1l_store_lock_give(&s_reconcile_lock);
    return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
}

bool d1l_channel_message_reconcile_pending(void)
{
    d1l_store_lock_take(&s_reconcile_lock);
    const bool pending = s_reconcile_pending;
    d1l_store_lock_give(&s_reconcile_lock);
    return pending;
}

esp_err_t d1l_channel_message_reconcile_if_due(uint32_t now_ms)
{
    const uint64_t observed_message_revision =
        d1l_message_store_stats().persistence_revision;
    d1l_store_lock_take(&s_reconcile_lock);
    const bool revision_changed = observed_message_revision !=
        s_last_reconciled_message_revision;
    const bool due = (!s_reconcile_pending && revision_changed) ||
        (s_reconcile_pending &&
         (s_last_retry_ms == 0U ||
          (uint32_t)(now_ms - s_last_retry_ms) >=
              D1L_CHANNEL_MESSAGE_RECONCILE_RETRY_MS));
    if (due) {
        s_last_retry_ms = now_ms;
    }
    d1l_store_lock_give(&s_reconcile_lock);
    return due ? d1l_channel_message_reconcile() : ESP_OK;
}
