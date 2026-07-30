#include "route_store_worker.h"

#include <stdbool.h>
#include <string.h>

#include "esp_timer.h"
#include "diagnostics/health_monitor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mesh/contact_store.h"
#include "mesh/dm_store.h"
#include "mesh/message_store.h"
#include "mesh/node_store.h"
#include "mesh/packet_log.h"
#include "mesh/read_state.h"
#include "mesh/route_store.h"
#include "platform/time_service.h"
#include "storage/retained_store_scheduler.h"

#define D1L_ROUTE_STORE_WORKER_INTERVAL_MS 1000U
/* This worker now owns the deepest Public/DM/packet/route persistence call
 * chain.  The previous route-only 4 KiB allocation overflowed on hardware as
 * soon as late-SD packet reconciliation ran.  ESP-IDF specifies task stack
 * depth and high-water marks in bytes, so keep a production-sized margin and
 * expose the measured floor through health diagnostics. */
#define D1L_ROUTE_STORE_WORKER_STACK_BYTES 12288U
#define D1L_ROUTE_STORE_WORKER_PRIORITY (tskIDLE_PRIORITY + 1U)
#define D1L_ROUTE_STORE_WORKER_QUEUE_LENGTH 2U
#define D1L_ROUTE_STORE_WORKER_POLL_MS 10U

typedef struct {
    uint32_t request_id;
    int64_t deadline_us;
} d1l_route_store_worker_request_t;

static QueueHandle_t s_request_queue;
static SemaphoreHandle_t s_request_mutex;
static SemaphoreHandle_t s_flush_mutex;
static TaskHandle_t s_worker_task;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_quiesce_owner_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_worker_starting;
static TaskHandle_t s_quiesce_owner;
static TaskHandle_t s_quiesce_requester;
static bool s_quiesce_preempt_requested;
static uint32_t s_next_request_id;
static uint32_t s_result_request_id;
static esp_err_t s_result;
static d1l_retained_store_worker_status_t s_worker_status;

static esp_err_t flush_messages(void *context)
{
    (void)context;
    return d1l_message_store_flush();
}

static esp_err_t flush_messages_if_due(void *context)
{
    (void)context;
    return d1l_message_store_flush_if_due();
}

static void observe_messages(
    void *context, d1l_retained_store_observation_t *out_observation)
{
    (void)context;
    const d1l_message_store_stats_t stats = d1l_message_store_stats();
    *out_observation = (d1l_retained_store_observation_t) {
        .revision = stats.persistence_revision,
        .commit_count = stats.persistence_commit_count,
        .failure_count = stats.persistence_fail_count,
        .dirty = stats.persistence_dirty,
        .reconcile_pending = stats.sd_primary_reconcile_pending,
    };
}

static esp_err_t flush_direct_messages(void *context)
{
    (void)context;
    return d1l_dm_store_flush();
}

static esp_err_t flush_direct_messages_if_due(void *context)
{
    (void)context;
    return d1l_dm_store_flush_if_due();
}

static void observe_direct_messages(
    void *context, d1l_retained_store_observation_t *out_observation)
{
    (void)context;
    const d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    *out_observation = (d1l_retained_store_observation_t) {
        .revision = stats.persistence_revision,
        .commit_count = stats.persistence_commit_count,
        .failure_count = stats.persistence_fail_count,
        .dirty = stats.persistence_dirty,
        .reconcile_pending = stats.sd_primary_reconcile_pending,
    };
}

static esp_err_t flush_packets(void *context)
{
    (void)context;
    return d1l_packet_log_flush();
}

static esp_err_t flush_packets_if_due(void *context)
{
    (void)context;
    return d1l_packet_log_flush_if_due();
}

static void observe_packets(
    void *context, d1l_retained_store_observation_t *out_observation)
{
    (void)context;
    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    *out_observation = (d1l_retained_store_observation_t) {
        .revision = stats.persistence_revision,
        .commit_count = stats.persistence_commit_count,
        .failure_count = stats.persistence_fail_count,
        .dirty = stats.persistence_dirty,
        .reconcile_pending = stats.sd_primary_reconcile_pending,
    };
}

static esp_err_t flush_routes(void *context)
{
    (void)context;
    return d1l_route_store_flush();
}

static esp_err_t flush_routes_if_due(void *context)
{
    (void)context;
    return d1l_route_store_flush_if_due();
}

static void observe_routes(
    void *context, d1l_retained_store_observation_t *out_observation)
{
    (void)context;
    const d1l_route_store_stats_t stats = d1l_route_store_stats();
    *out_observation = (d1l_retained_store_observation_t) {
        .revision = stats.persistence_revision,
        .commit_count = stats.persistence_commit_count,
        .failure_count = stats.persistence_fail_count,
        .dirty = stats.persistence_dirty,
        .reconcile_pending = stats.sd_primary_reconcile_pending,
    };
}

static esp_err_t flush_contacts(void *context)
{
    (void)context;
    return d1l_contact_store_flush();
}

static esp_err_t flush_contacts_if_due(void *context)
{
    (void)context;
    return d1l_contact_store_flush_if_due();
}

static void observe_contacts(
    void *context, d1l_retained_store_observation_t *out_observation)
{
    (void)context;
    const d1l_contact_store_stats_t stats = d1l_contact_store_stats();
    *out_observation = (d1l_retained_store_observation_t) {
        .revision = stats.persistence_revision,
        .commit_count = stats.persistence_commit_count,
        .failure_count = stats.persistence_fail_count,
        .dirty = stats.persistence_dirty,
        .reconcile_pending = stats.sd_primary_reconcile_pending,
    };
}

static esp_err_t flush_read_state(void *context)
{
    (void)context;
    return d1l_read_state_flush();
}

static esp_err_t flush_read_state_if_due(void *context)
{
    (void)context;
    return d1l_read_state_flush_if_due();
}

static void observe_read_state(
    void *context, d1l_retained_store_observation_t *out_observation)
{
    (void)context;
    const d1l_read_state_stats_t stats = d1l_read_state_stats();
    *out_observation = (d1l_retained_store_observation_t) {
        .revision = stats.persistence_revision,
        .commit_count = stats.persistence_commit_count,
        .failure_count = stats.persistence_fail_count,
        .dirty = stats.persistence_dirty,
        .reconcile_pending = stats.sd_primary_reconcile_pending,
    };
}

static esp_err_t flush_time_checkpoint(void *context)
{
    (void)context;
    return d1l_time_service_wall_checkpoint_flush();
}

static esp_err_t flush_time_checkpoint_if_due(void *context)
{
    (void)context;
    return d1l_time_service_wall_checkpoint_flush_if_due();
}

static void observe_time_checkpoint(
    void *context, d1l_retained_store_observation_t *out_observation)
{
    (void)context;
    d1l_time_service_status_t status;
    d1l_time_service_status(&status);
    *out_observation = (d1l_retained_store_observation_t) {
        .revision = status.wall_checkpoint.revision,
        .commit_count = status.wall_checkpoint_write_count,
        .failure_count = status.wall_checkpoint_failure_count,
        .dirty = status.wall_checkpoint_pending,
        .reconcile_pending = false,
    };
}

static esp_err_t flush_nodes(void *context)
{
    (void)context;
    return d1l_node_store_flush();
}

static esp_err_t flush_nodes_if_due(void *context)
{
    (void)context;
    return d1l_node_store_flush_if_due();
}

static void observe_nodes(
    void *context, d1l_retained_store_observation_t *out_observation)
{
    (void)context;
    const d1l_node_store_stats_t stats = d1l_node_store_stats();
    *out_observation = (d1l_retained_store_observation_t) {
        .revision = stats.persistence_revision,
        .commit_count = stats.persistence_commit_count,
        .failure_count = stats.persistence_fail_count,
        .dirty = stats.persistence_dirty,
        .reconcile_pending = stats.sd_primary_reconcile_pending,
    };
}

static const d1l_retained_store_descriptor_t s_retained_stores[] = {
    {
        .kind = D1L_RETAINED_STORE_MESSAGES,
        .name = "messages",
        .flush = flush_messages,
        .flush_if_due = flush_messages_if_due,
        .observe = observe_messages,
    },
    {
        .kind = D1L_RETAINED_STORE_DIRECT_MESSAGES,
        .name = "direct_messages",
        .flush = flush_direct_messages,
        .flush_if_due = flush_direct_messages_if_due,
        .observe = observe_direct_messages,
    },
    {
        .kind = D1L_RETAINED_STORE_PACKETS,
        .name = "packets",
        .flush = flush_packets,
        .flush_if_due = flush_packets_if_due,
        .observe = observe_packets,
    },
    {
        .kind = D1L_RETAINED_STORE_ROUTES,
        .name = "routes",
        .flush = flush_routes,
        .flush_if_due = flush_routes_if_due,
        .observe = observe_routes,
    },
    {
        .kind = D1L_RETAINED_STORE_CONTACTS,
        .name = "contacts",
        .flush = flush_contacts,
        .flush_if_due = flush_contacts_if_due,
        .observe = observe_contacts,
    },
    {
        .kind = D1L_RETAINED_STORE_READ_STATE,
        .name = "read_state",
        .flush = flush_read_state,
        .flush_if_due = flush_read_state_if_due,
        .observe = observe_read_state,
    },
    {
        .kind = D1L_RETAINED_STORE_TIME_CHECKPOINT,
        .name = "time_checkpoint",
        .flush = flush_time_checkpoint,
        .flush_if_due = flush_time_checkpoint_if_due,
        .observe = observe_time_checkpoint,
    },
    {
        .kind = D1L_RETAINED_STORE_NODES,
        .name = "nodes",
        .flush = flush_nodes,
        .flush_if_due = flush_nodes_if_due,
        .observe = observe_nodes,
    },
};

static int64_t scheduler_clock(void *context)
{
    (void)context;
    return esp_timer_get_time();
}

static bool worker_quiesce_requested(void)
{
    bool requested;
    portENTER_CRITICAL(&s_quiesce_owner_mux);
    requested = s_quiesce_requester != NULL &&
                s_quiesce_preempt_requested;
    portEXIT_CRITICAL(&s_quiesce_owner_mux);
    return requested;
}

bool d1l_route_store_persistence_should_yield(void)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    TaskHandle_t requester;
    TaskHandle_t owner;
    bool preempt_requested;
    portENTER_CRITICAL(&s_quiesce_owner_mux);
    requester = s_quiesce_requester;
    owner = s_quiesce_owner;
    preempt_requested = s_quiesce_preempt_requested;
    portEXIT_CRITICAL(&s_quiesce_owner_mux);
    return preempt_requested && requester != NULL &&
           current != requester && current != owner;
}

static bool scheduler_cancel_requested(void *context)
{
    (void)context;
    return worker_quiesce_requested();
}

static TickType_t absolute_deadline_remaining_ticks(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0U;
    }
    const TickType_t max_finite_ticks =
        portMAX_DELAY > 1U ? portMAX_DELAY - 1U : 0U;
    return (TickType_t)d1l_retained_store_finite_wait_ticks(
        (uint64_t)remaining_us, configTICK_RATE_HZ,
        (uint32_t)max_finite_ticks);
}

static void set_worker_running(bool running, bool force, int64_t deadline_us)
{
    portENTER_CRITICAL(&s_state_mux);
    s_worker_status.running = running;
    s_worker_status.active_force = force;
    s_worker_status.active_deadline_us = deadline_us;
    s_worker_status.active_store_started_us = 0;
    s_worker_status.active_store_index = 0U;
    s_worker_status.active_store_name[0] = '\0';
    portEXIT_CRITICAL(&s_state_mux);
}

static void publish_store_progress(
    void *context, size_t store_index,
    const d1l_retained_store_result_t *result, bool starting)
{
    (void)context;
    portENTER_CRITICAL(&s_state_mux);
    if (starting) {
        s_worker_status.active_store_index = store_index;
        s_worker_status.active_store_started_us = result->started_us;
        strncpy(s_worker_status.active_store_name, result->name,
                sizeof(s_worker_status.active_store_name) - 1U);
        s_worker_status.active_store_name[
            sizeof(s_worker_status.active_store_name) - 1U] = '\0';
    } else {
        s_worker_status.active_store_started_us = 0;
        s_worker_status.active_store_name[0] = '\0';
    }
    portEXIT_CRITICAL(&s_state_mux);
}

static void publish_pass(const d1l_retained_store_pass_t *pass)
{
    portENTER_CRITICAL(&s_state_mux);
    s_worker_status.running = false;
    s_worker_status.active_force = false;
    s_worker_status.active_deadline_us = 0;
    s_worker_status.active_store_started_us = 0;
    s_worker_status.active_store_name[0] = '\0';
    s_worker_status.pass_count++;
    if (pass->force) {
        s_worker_status.forced_pass_count++;
    } else {
        s_worker_status.background_pass_count++;
    }
    if (pass->deadline_exhausted) {
        s_worker_status.deadline_exhausted_count++;
    }
    if (pass->quiesce_cancelled) {
        s_worker_status.quiesce_cancelled_count++;
    }
    s_worker_status.last_pass = *pass;
    portEXIT_CRITICAL(&s_state_mux);
}

static esp_err_t flush_retained_stores_locked(bool force, int64_t deadline_us)
{
    d1l_retained_store_pass_t pass = {
        .force = force,
        .deadline_us = deadline_us,
        .started_us = esp_timer_get_time(),
    };
    set_worker_running(true, force, deadline_us);
    if (!s_flush_mutex) {
        pass.finished_us = esp_timer_get_time();
        pass.result = ESP_ERR_INVALID_STATE;
        publish_pass(&pass);
        return pass.result;
    }

    TickType_t wait_ticks = portMAX_DELAY;
    if (force) {
        wait_ticks = absolute_deadline_remaining_ticks(deadline_us);
        if (wait_ticks == 0U) {
            pass.finished_us = esp_timer_get_time();
            pass.result = ESP_ERR_TIMEOUT;
            pass.deadline_exhausted = true;
            publish_pass(&pass);
            return pass.result;
        }
    }
    if (xSemaphoreTake(s_flush_mutex, wait_ticks) != pdTRUE) {
        pass.finished_us = esp_timer_get_time();
        pass.result = ESP_ERR_TIMEOUT;
        pass.deadline_exhausted = force;
        publish_pass(&pass);
        return pass.result;
    }

    const d1l_retained_store_scheduler_options_t options = {
        .force = force,
        .deadline_us = deadline_us,
        .clock = scheduler_clock,
        .cancel_requested = scheduler_cancel_requested,
        .progress = publish_store_progress,
    };
    const esp_err_t ret = d1l_retained_store_scheduler_run(
        s_retained_stores,
        sizeof(s_retained_stores) / sizeof(s_retained_stores[0]),
        &options, &pass);
    (void)xSemaphoreGive(s_flush_mutex);
    publish_pass(&pass);
    return ret;
}

static TickType_t quiesce_remaining_ticks(int64_t started_us,
                                          uint32_t timeout_ms)
{
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    const int64_t timeout_us = (int64_t)timeout_ms * 1000LL;
    if (elapsed_us >= timeout_us) {
        return 0;
    }
    const TickType_t max_finite_ticks =
        portMAX_DELAY > 1U ? portMAX_DELAY - 1U : 0U;
    return (TickType_t)d1l_retained_store_finite_wait_ticks(
        (uint64_t)(timeout_us - elapsed_us), configTICK_RATE_HZ,
        (uint32_t)max_finite_ticks);
}

static void publish_result(uint32_t request_id, esp_err_t result)
{
    portENTER_CRITICAL(&s_state_mux);
    s_result_request_id = request_id;
    s_result = result;
    portEXIT_CRITICAL(&s_state_mux);
}

static void route_store_worker_task(void *arg)
{
    (void)arg;
    d1l_route_store_worker_request_t request = {0};
    for (;;) {
        if (xQueueReceive(s_request_queue, &request,
                          pdMS_TO_TICKS(D1L_ROUTE_STORE_WORKER_INTERVAL_MS)) == pdTRUE) {
            publish_result(
                request.request_id,
                flush_retained_stores_locked(true, request.deadline_us));
        } else {
            (void)flush_retained_stores_locked(false, 0);
        }
    }
}

esp_err_t d1l_route_store_worker_start(void)
{
    portENTER_CRITICAL(&s_state_mux);
    const bool already_started = s_worker_task != NULL;
    const bool already_starting = s_worker_starting;
    if (!already_started && !already_starting) {
        s_worker_starting = true;
    }
    portEXIT_CRITICAL(&s_state_mux);
    if (already_started) {
        return ESP_OK;
    }
    if (already_starting) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_request_mutex) {
        s_request_mutex = xSemaphoreCreateMutex();
        if (!s_request_mutex) {
            portENTER_CRITICAL(&s_state_mux);
            s_worker_starting = false;
            portEXIT_CRITICAL(&s_state_mux);
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_flush_mutex) {
        s_flush_mutex = xSemaphoreCreateMutex();
        if (!s_flush_mutex) {
            portENTER_CRITICAL(&s_state_mux);
            s_worker_starting = false;
            portEXIT_CRITICAL(&s_state_mux);
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_request_queue) {
        s_request_queue = xQueueCreate(D1L_ROUTE_STORE_WORKER_QUEUE_LENGTH,
                                       sizeof(d1l_route_store_worker_request_t));
        if (!s_request_queue) {
            portENTER_CRITICAL(&s_state_mux);
            s_worker_starting = false;
            portEXIT_CRITICAL(&s_state_mux);
            return ESP_ERR_NO_MEM;
        }
    }
    TaskHandle_t created_task = NULL;
    if (xTaskCreate(route_store_worker_task, "route_persist",
                    D1L_ROUTE_STORE_WORKER_STACK_BYTES, NULL,
                    D1L_ROUTE_STORE_WORKER_PRIORITY,
                    &created_task) != pdPASS) {
        portENTER_CRITICAL(&s_state_mux);
        s_worker_starting = false;
        portEXIT_CRITICAL(&s_state_mux);
        return ESP_ERR_NO_MEM;
    }
    d1l_health_monitor_register_retained_task(created_task);
    portENTER_CRITICAL(&s_state_mux);
    s_worker_task = created_task;
    s_worker_starting = false;
    portEXIT_CRITICAL(&s_state_mux);
    return ESP_OK;
}

esp_err_t d1l_route_store_worker_force_flush(uint32_t timeout_ms)
{
    if (timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t started_us = esp_timer_get_time();
    const int64_t deadline_us =
        started_us + (int64_t)timeout_ms * 1000LL;
    esp_err_t ret = d1l_route_store_worker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    TickType_t remaining_ticks = absolute_deadline_remaining_ticks(deadline_us);
    if (remaining_ticks == 0U ||
        xSemaphoreTake(s_request_mutex, remaining_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (esp_timer_get_time() >= deadline_us) {
        (void)xSemaphoreGive(s_request_mutex);
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_state_mux);
    uint32_t request_id = ++s_next_request_id;
    if (request_id == 0U) {
        request_id = ++s_next_request_id;
    }
    portEXIT_CRITICAL(&s_state_mux);
    const d1l_route_store_worker_request_t request = {
        .request_id = request_id,
        .deadline_us = deadline_us,
    };
    if (xQueueSend(s_request_queue, &request, 0) != pdTRUE) {
        (void)xSemaphoreGive(s_request_mutex);
        return ESP_ERR_TIMEOUT;
    }

    for (;;) {
        uint32_t result_request_id;
        esp_err_t result;
        portENTER_CRITICAL(&s_state_mux);
        result_request_id = s_result_request_id;
        result = s_result;
        portEXIT_CRITICAL(&s_state_mux);
        if (result_request_id == request_id) {
            (void)xSemaphoreGive(s_request_mutex);
            return result;
        }
        remaining_ticks = absolute_deadline_remaining_ticks(deadline_us);
        if (remaining_ticks == 0U) {
            (void)xSemaphoreGive(s_request_mutex);
            return ESP_ERR_TIMEOUT;
        }
        TickType_t poll_ticks =
            pdMS_TO_TICKS(D1L_ROUTE_STORE_WORKER_POLL_MS);
        if (poll_ticks == 0U) {
            poll_ticks = 1U;
        }
        vTaskDelay(poll_ticks < remaining_ticks ? poll_ticks : remaining_ticks);
    }
}

static esp_err_t route_store_worker_quiesce_begin(uint32_t timeout_ms,
                                                   bool preempt)
{
    if (timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t started_us = esp_timer_get_time();
    esp_err_t ret = d1l_route_store_worker_start();
    if (ret != ESP_OK) {
        return ret;
    }

    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    bool request_registered = false;
    portENTER_CRITICAL(&s_quiesce_owner_mux);
    if (s_quiesce_owner == NULL && s_quiesce_requester == NULL) {
        s_quiesce_requester = current;
        s_quiesce_preempt_requested = preempt;
        request_registered = true;
    }
    portEXIT_CRITICAL(&s_quiesce_owner_mux);
    if (!request_registered) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t ticks = quiesce_remaining_ticks(started_us, timeout_ms);
    if (ticks == 0U || xSemaphoreTake(s_request_mutex, ticks) != pdTRUE) {
        portENTER_CRITICAL(&s_quiesce_owner_mux);
        if (s_quiesce_requester == current) {
            s_quiesce_requester = NULL;
            s_quiesce_preempt_requested = false;
        }
        portEXIT_CRITICAL(&s_quiesce_owner_mux);
        return ESP_ERR_TIMEOUT;
    }
    ticks = quiesce_remaining_ticks(started_us, timeout_ms);
    if (ticks == 0U || xSemaphoreTake(s_flush_mutex, ticks) != pdTRUE) {
        portENTER_CRITICAL(&s_quiesce_owner_mux);
        if (s_quiesce_requester == current) {
            s_quiesce_requester = NULL;
            s_quiesce_preempt_requested = false;
        }
        portEXIT_CRITICAL(&s_quiesce_owner_mux);
        (void)xSemaphoreGive(s_request_mutex);
        return ESP_ERR_TIMEOUT;
    }
    if (esp_timer_get_time() - started_us >= (int64_t)timeout_ms * 1000LL) {
        portENTER_CRITICAL(&s_quiesce_owner_mux);
        if (s_quiesce_requester == current) {
            s_quiesce_requester = NULL;
            s_quiesce_preempt_requested = false;
        }
        portEXIT_CRITICAL(&s_quiesce_owner_mux);
        (void)xSemaphoreGive(s_flush_mutex);
        (void)xSemaphoreGive(s_request_mutex);
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_quiesce_owner_mux);
    s_quiesce_owner = current;
    /* Wait-mode must not cancel the worker pass whose flush mutex we are
     * acquiring. Once ownership is established, however, direct Public/DM/
     * packet persistence does not hold that mutex and must yield before the
     * owner begins bridge status or mount traffic. */
    s_quiesce_preempt_requested = true;
    portEXIT_CRITICAL(&s_quiesce_owner_mux);
    return ESP_OK;
}

esp_err_t d1l_route_store_worker_quiesce_begin(uint32_t timeout_ms)
{
    return route_store_worker_quiesce_begin(timeout_ms, true);
}

esp_err_t d1l_route_store_worker_quiesce_wait_begin(uint32_t timeout_ms)
{
    return route_store_worker_quiesce_begin(timeout_ms, false);
}

void d1l_route_store_worker_quiesce_end(void)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_quiesce_owner_mux);
    if (s_quiesce_owner != current) {
        portEXIT_CRITICAL(&s_quiesce_owner_mux);
        return;
    }
    s_quiesce_owner = NULL;
    s_quiesce_requester = NULL;
    s_quiesce_preempt_requested = false;
    portEXIT_CRITICAL(&s_quiesce_owner_mux);
    (void)xSemaphoreGive(s_flush_mutex);
    (void)xSemaphoreGive(s_request_mutex);
}

void d1l_retained_store_worker_status(
    d1l_retained_store_worker_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    portENTER_CRITICAL(&s_state_mux);
    *out_status = s_worker_status;
    portEXIT_CRITICAL(&s_state_mux);
}
