#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "diagnostics/health_monitor.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mesh/contact_store.h"
#include "mesh/dm_store.h"
#include "mesh/message_store.h"
#include "mesh/node_store.h"
#include "mesh/packet_log.h"
#include "mesh/route_store.h"
#include "mesh/route_store_worker.h"
#include "platform/time_service.h"

struct d1l_native_semaphore {
    pthread_mutex_t mutex;
};

struct d1l_native_queue {
    pthread_mutex_t mutex;
    size_t length;
    size_t item_size;
    size_t count;
    size_t head;
    size_t tail;
    unsigned char *items;
};

typedef struct {
    TaskFunction_t task;
    void *argument;
} native_task_start_t;

typedef struct {
    uint64_t revision;
    uint32_t commits;
    uint32_t failures;
    uint32_t flush_calls;
    bool dirty;
    bool reconcile_pending;
} native_store_t;

static pthread_mutex_t s_store_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_store_condition = PTHREAD_COND_INITIALIZER;
static native_store_t s_stores[7];
static bool s_block_message;
static bool s_message_entered;
static bool s_release_message;

static int64_t native_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    static bool initialized;
    LARGE_INTEGER counter;
    if (!initialized) {
        assert(QueryPerformanceFrequency(&frequency));
        initialized = true;
    }
    assert(QueryPerformanceCounter(&counter));
    const int64_t whole_seconds = counter.QuadPart / frequency.QuadPart;
    const int64_t remainder = counter.QuadPart % frequency.QuadPart;
    return whole_seconds * 1000000LL +
           (remainder * 1000000LL) / frequency.QuadPart;
#else
    struct timespec value;
    assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (int64_t)value.tv_sec * 1000000LL + value.tv_nsec / 1000LL;
#endif
}

static void native_sleep_ms(uint32_t milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    const struct timespec delay = {
        .tv_sec = milliseconds / 1000U,
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    (void)nanosleep(&delay, NULL);
#endif
}

int64_t esp_timer_get_time(void)
{
    return native_now_us();
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct d1l_native_semaphore *semaphore = calloc(1U, sizeof(*semaphore));
    if (!semaphore || pthread_mutex_init(&semaphore->mutex, NULL) != 0) {
        free(semaphore);
        return NULL;
    }
    return semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                          TickType_t ticks_to_wait)
{
    if (!semaphore) {
        return pdFALSE;
    }
    if (ticks_to_wait == portMAX_DELAY) {
        return pthread_mutex_lock(&semaphore->mutex) == 0 ? pdTRUE : pdFALSE;
    }
    const int64_t deadline_us =
        native_now_us() + (int64_t)ticks_to_wait * 1000LL;
    for (;;) {
        const int result = pthread_mutex_trylock(&semaphore->mutex);
        if (result == 0) {
            return pdTRUE;
        }
        assert(result == EBUSY);
        if (ticks_to_wait == 0U || native_now_us() >= deadline_us) {
            return pdFALSE;
        }
        native_sleep_ms(1U);
    }
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return semaphore && pthread_mutex_unlock(&semaphore->mutex) == 0 ?
        pdTRUE : pdFALSE;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    struct d1l_native_queue *queue = calloc(1U, sizeof(*queue));
    if (!queue) {
        return NULL;
    }
    queue->items = calloc(length, item_size);
    if (!queue->items || pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue->items);
        free(queue);
        return NULL;
    }
    queue->length = length;
    queue->item_size = item_size;
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    if (!queue || !item) {
        return pdFALSE;
    }
    assert(pthread_mutex_lock(&queue->mutex) == 0);
    if (queue->count == queue->length) {
        assert(pthread_mutex_unlock(&queue->mutex) == 0);
        return pdFALSE;
    }
    memcpy(queue->items + queue->tail * queue->item_size,
           item, queue->item_size);
    queue->tail = (queue->tail + 1U) % queue->length;
    queue->count++;
    assert(pthread_mutex_unlock(&queue->mutex) == 0);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t ticks_to_wait)
{
    if (!queue || !item) {
        return pdFALSE;
    }
    const int64_t deadline_us = ticks_to_wait == portMAX_DELAY ? INT64_MAX :
        native_now_us() + (int64_t)ticks_to_wait * 1000LL;
    for (;;) {
        assert(pthread_mutex_lock(&queue->mutex) == 0);
        if (queue->count > 0U) {
            memcpy(item, queue->items + queue->head * queue->item_size,
                   queue->item_size);
            queue->head = (queue->head + 1U) % queue->length;
            queue->count--;
            assert(pthread_mutex_unlock(&queue->mutex) == 0);
            return pdTRUE;
        }
        assert(pthread_mutex_unlock(&queue->mutex) == 0);
        if (ticks_to_wait == 0U || native_now_us() >= deadline_us) {
            return pdFALSE;
        }
        native_sleep_ms(1U);
    }
}

static void *native_task_entry(void *argument)
{
    native_task_start_t start = *(native_task_start_t *)argument;
    free(argument);
    start.task(start.argument);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t task, const char *name,
                       uint32_t stack_depth, void *argument,
                       UBaseType_t priority, TaskHandle_t *out_handle)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    native_task_start_t *start = malloc(sizeof(*start));
    if (!start) {
        return pdFALSE;
    }
    start->task = task;
    start->argument = argument;
    pthread_t thread;
    if (pthread_create(&thread, NULL, native_task_entry, start) != 0) {
        free(start);
        return pdFALSE;
    }
    assert(pthread_detach(thread) == 0);
    if (out_handle) {
        *out_handle = start;
    }
    return pdPASS;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    static _Thread_local int task_token;
    return &task_token;
}

void vTaskDelay(TickType_t ticks)
{
    native_sleep_ms(ticks);
}

void d1l_health_monitor_register_retained_task(TaskHandle_t task)
{
    (void)task;
}

static esp_err_t native_store_flush(size_t index)
{
    assert(index < 7U);
    assert(pthread_mutex_lock(&s_store_mutex) == 0);
    native_store_t *store = &s_stores[index];
    store->flush_calls++;
    if (index == 0U && s_block_message) {
        s_message_entered = true;
        assert(pthread_cond_broadcast(&s_store_condition) == 0);
        while (!s_release_message) {
            assert(pthread_cond_wait(&s_store_condition,
                                     &s_store_mutex) == 0);
        }
    }
    store->commits++;
    store->dirty = false;
    assert(pthread_mutex_unlock(&s_store_mutex) == 0);
    return ESP_OK;
}

static native_store_t native_store_stats(size_t index)
{
    assert(index < 7U);
    assert(pthread_mutex_lock(&s_store_mutex) == 0);
    const native_store_t stats = s_stores[index];
    assert(pthread_mutex_unlock(&s_store_mutex) == 0);
    return stats;
}

esp_err_t d1l_message_store_flush(void) { return native_store_flush(0U); }
esp_err_t d1l_message_store_flush_if_due(void) { return native_store_flush(0U); }
esp_err_t d1l_dm_store_flush(void) { return native_store_flush(1U); }
esp_err_t d1l_dm_store_flush_if_due(void) { return native_store_flush(1U); }
esp_err_t d1l_packet_log_flush(void) { return native_store_flush(2U); }
esp_err_t d1l_packet_log_flush_if_due(void) { return native_store_flush(2U); }
esp_err_t d1l_route_store_flush(void) { return native_store_flush(3U); }
esp_err_t d1l_route_store_flush_if_due(void) { return native_store_flush(3U); }
esp_err_t d1l_contact_store_flush(void) { return native_store_flush(4U); }
esp_err_t d1l_contact_store_flush_if_due(void) { return native_store_flush(4U); }
esp_err_t d1l_time_service_wall_checkpoint_flush(void)
{
    return native_store_flush(5U);
}
esp_err_t d1l_time_service_wall_checkpoint_flush_if_due(void)
{
    return native_store_flush(5U);
}
esp_err_t d1l_node_store_flush(void) { return native_store_flush(6U); }
esp_err_t d1l_node_store_flush_if_due(void) { return native_store_flush(6U); }

void d1l_time_service_status(d1l_time_service_status_t *out_status)
{
    assert(out_status != NULL);
    const native_store_t stats = native_store_stats(5U);
    *out_status = (d1l_time_service_status_t) {
        .wall_checkpoint = {
            .revision = (uint32_t)stats.revision,
        },
        .wall_checkpoint_write_count = stats.commits,
        .wall_checkpoint_failure_count = stats.failures,
        .wall_checkpoint_pending = stats.dirty,
    };
}

d1l_message_store_stats_t d1l_message_store_stats(void)
{
    const native_store_t stats = native_store_stats(0U);
    return (d1l_message_store_stats_t) {
        .persistence_revision = stats.revision,
        .persistence_commit_count = stats.commits,
        .persistence_fail_count = stats.failures,
        .persistence_dirty = stats.dirty,
        .sd_primary_reconcile_pending = stats.reconcile_pending,
    };
}

d1l_dm_store_stats_t d1l_dm_store_stats(void)
{
    const native_store_t stats = native_store_stats(1U);
    return (d1l_dm_store_stats_t) {
        .persistence_revision = stats.revision,
        .persistence_commit_count = stats.commits,
        .persistence_fail_count = stats.failures,
        .persistence_dirty = stats.dirty,
        .sd_primary_reconcile_pending = stats.reconcile_pending,
    };
}

d1l_packet_log_stats_t d1l_packet_log_stats(void)
{
    const native_store_t stats = native_store_stats(2U);
    return (d1l_packet_log_stats_t) {
        .persistence_revision = stats.revision,
        .persistence_commit_count = stats.commits,
        .persistence_fail_count = stats.failures,
        .persistence_dirty = stats.dirty,
        .sd_primary_reconcile_pending = stats.reconcile_pending,
    };
}

d1l_route_store_stats_t d1l_route_store_stats(void)
{
    const native_store_t stats = native_store_stats(3U);
    return (d1l_route_store_stats_t) {
        .persistence_revision = stats.revision,
        .persistence_commit_count = stats.commits,
        .persistence_fail_count = stats.failures,
        .persistence_dirty = stats.dirty,
        .sd_primary_reconcile_pending = stats.reconcile_pending,
    };
}

d1l_contact_store_stats_t d1l_contact_store_stats(void)
{
    const native_store_t stats = native_store_stats(4U);
    return (d1l_contact_store_stats_t) {
        .persistence_revision = stats.revision,
        .persistence_commit_count = stats.commits,
        .persistence_fail_count = stats.failures,
        .persistence_dirty = stats.dirty,
    };
}

d1l_node_store_stats_t d1l_node_store_stats(void)
{
    const native_store_t stats = native_store_stats(6U);
    return (d1l_node_store_stats_t) {
        .persistence_revision = stats.revision,
        .persistence_commit_count = stats.commits,
        .persistence_fail_count = stats.failures,
        .persistence_dirty = stats.dirty,
        .sd_primary_reconcile_pending = stats.reconcile_pending,
    };
}

typedef struct {
    uint32_t timeout_ms;
    esp_err_t result;
} force_call_t;

static void *force_call_thread(void *argument)
{
    force_call_t *call = argument;
    call->result = d1l_route_store_worker_force_flush(call->timeout_ms);
    return NULL;
}

static void wait_for_message_entry(void)
{
    const int64_t deadline_us = native_now_us() + 1000000LL;
    for (;;) {
        assert(pthread_mutex_lock(&s_store_mutex) == 0);
        const bool entered = s_message_entered;
        assert(pthread_mutex_unlock(&s_store_mutex) == 0);
        if (entered) {
            return;
        }
        assert(native_now_us() < deadline_us);
        native_sleep_ms(1U);
    }
}

static d1l_retained_store_worker_status_t wait_for_passes(uint32_t passes)
{
    const int64_t deadline_us = native_now_us() + 1000000LL;
    for (;;) {
        d1l_retained_store_worker_status_t status = {0};
        d1l_retained_store_worker_status(&status);
        if (!status.running && status.pass_count >= passes) {
            return status;
        }
        assert(native_now_us() < deadline_us);
        native_sleep_ms(1U);
    }
}

static uint32_t store_flush_calls(size_t index)
{
    return native_store_stats(index).flush_calls;
}

int main(void)
{
    for (size_t i = 0U; i < 7U; ++i) {
        s_stores[i].dirty = true;
        s_stores[i].revision = i + 1U;
    }
    s_block_message = true;

    force_call_t first = {.timeout_ms = 80U, .result = ESP_OK};
    pthread_t first_thread;
    assert(pthread_create(&first_thread, NULL, force_call_thread, &first) == 0);
    wait_for_message_entry();
    assert(pthread_join(first_thread, NULL) == 0);
    assert(first.result == ESP_ERR_TIMEOUT);

    d1l_retained_store_worker_status_t active = {0};
    d1l_retained_store_worker_status(&active);
    assert(active.running);
    assert(active.active_force);
    assert(strcmp(active.active_store_name, "messages") == 0);
    assert(active.active_deadline_us <= native_now_us());

    /* The first timed-out caller released the request mutex, so this request
     * is queued behind the still-running callback and expires in the queue. */
    force_call_t queued = {.timeout_ms = 20U, .result = ESP_OK};
    pthread_t queued_thread;
    assert(pthread_create(&queued_thread, NULL, force_call_thread, &queued) == 0);
    assert(pthread_join(queued_thread, NULL) == 0);
    assert(queued.result == ESP_ERR_TIMEOUT);

    assert(pthread_mutex_lock(&s_store_mutex) == 0);
    s_release_message = true;
    s_block_message = false;
    assert(pthread_cond_broadcast(&s_store_condition) == 0);
    assert(pthread_mutex_unlock(&s_store_mutex) == 0);

    d1l_retained_store_worker_status_t expired = wait_for_passes(2U);
    assert(expired.last_pass.deadline_exhausted);
    assert(expired.last_pass.attempted_count == 0U);
    assert(store_flush_calls(0U) == 1U);
    assert(store_flush_calls(1U) == 0U);
    assert(store_flush_calls(2U) == 0U);
    assert(store_flush_calls(3U) == 0U);
    assert(store_flush_calls(4U) == 0U);
    assert(store_flush_calls(5U) == 0U);
    assert(store_flush_calls(6U) == 0U);

    assert(d1l_route_store_worker_force_flush(1000U) == ESP_OK);
    assert(store_flush_calls(0U) == 2U);
    assert(store_flush_calls(1U) == 1U);
    assert(store_flush_calls(2U) == 1U);
    assert(store_flush_calls(3U) == 1U);
    assert(store_flush_calls(4U) == 1U);
    assert(store_flush_calls(5U) == 1U);
    assert(store_flush_calls(6U) == 1U);

    /* A quiesce owner holds both request and flush locks. A competing forced
     * caller times out, then quiesce_end releases both for the next request. */
    assert(d1l_route_store_worker_quiesce_begin(500U) == ESP_OK);
    force_call_t held = {.timeout_ms = 20U, .result = ESP_OK};
    pthread_t held_thread;
    assert(pthread_create(&held_thread, NULL, force_call_thread, &held) == 0);
    assert(pthread_join(held_thread, NULL) == 0);
    assert(held.result == ESP_ERR_TIMEOUT);
    assert(!d1l_route_store_persistence_should_yield());
    d1l_route_store_worker_quiesce_end();

    assert(d1l_route_store_worker_force_flush(1000U) == ESP_OK);
    assert(store_flush_calls(0U) == 3U);
    assert(store_flush_calls(1U) == 2U);
    assert(store_flush_calls(2U) == 2U);
    assert(store_flush_calls(3U) == 2U);
    assert(store_flush_calls(4U) == 2U);
    assert(store_flush_calls(5U) == 2U);
    assert(store_flush_calls(6U) == 2U);

    puts("native retained-store worker lifecycle: ok");
    return 0;
}
