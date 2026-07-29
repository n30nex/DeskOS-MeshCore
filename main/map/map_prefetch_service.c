#include "map_prefetch_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app/settings_model.h"
#include "comms/connectivity_manager.h"
#include "map/map_prefetch_plan.h"
#include "map/map_view_service.h"
#include "mesh/node_store.h"
#include "storage/map_tile_store.h"
#include "storage/retained_blob_store.h"
#include "storage/storage_status.h"

#define D1L_MAP_PREFETCH_WORKER_STACK_BYTES \
    D1L_MAP_SHARED_WORKER_STACK_BYTES
#define D1L_MAP_PREFETCH_WORKER_PRIORITY (tskIDLE_PRIORITY + 1U)
#define D1L_MAP_VISIBLE_WORKER_PRIORITY 2U
#define D1L_MAP_PREFETCH_POLL_MS 5000U
#define D1L_MAP_PREFETCH_INTER_TILE_PAUSE_MS 25U
#define D1L_MAP_PREFETCH_ERROR_BACKOFF_SEC 30U
#define D1L_MAP_PREFETCH_DEFAULT_RATE_BACKOFF_SEC 300U

typedef struct {
    uint32_t marker_generation;
    uint32_t storage_backend_generation;
    uint32_t storage_capacity_kb;
    uint32_t average_tile_bytes;
    uint32_t cache_budget_mb;
    int32_t center_lat_e7;
    int32_t center_lon_e7;
    int32_t viewport_lat_e6;
    int32_t viewport_lon_e6;
    uint8_t provider_max_zoom;
    bool viewport_valid;
    char source_id[D1L_MAP_PROVIDER_SOURCE_ID_MAX + 1U];
} d1l_map_prefetch_key_t;

typedef struct {
    uint32_t marker_generation;
    char source_id[D1L_MAP_PROVIDER_SOURCE_ID_MAX + 1U];
} d1l_map_prefetch_continue_t;

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_map_prefetch_status_t s_status;
static bool s_starting;
static TaskHandle_t s_worker;
static SemaphoreHandle_t s_wake_lock;
static d1l_node_marker_t *s_markers;
static d1l_map_prefetch_point_t *s_points;
static uint8_t *s_tile_buffer;
static d1l_map_prefetch_key_t s_completed_key;
static bool s_completed_key_valid;
static int64_t s_backoff_until_us;
static uint64_t s_network_requests_total;

static void set_phase(d1l_map_prefetch_status_t *status,
                      const char *phase,
                      const char *message)
{
    if (!status) {
        return;
    }
    snprintf(status->phase, sizeof(status->phase), "%s",
             phase ? phase : "unknown");
    snprintf(status->message, sizeof(status->message), "%s",
             message ? message : "");
}

static void publish_status(
    const d1l_map_prefetch_status_t *status)
{
    if (!status) {
        return;
    }
    portENTER_CRITICAL(&s_status_lock);
    s_status = *status;
    s_status.network_requests = s_network_requests_total;
    s_status.worker_stack_bytes =
        D1L_MAP_PREFETCH_WORKER_STACK_BYTES;
    s_status.worker_stack_free_bytes =
        (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    portEXIT_CRITICAL(&s_status_lock);
}

void d1l_map_prefetch_service_status(
    d1l_map_prefetch_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    portENTER_CRITICAL(&s_status_lock);
    *out_status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}

static bool key_equal(const d1l_map_prefetch_key_t *left,
                      const d1l_map_prefetch_key_t *right)
{
    return left && right &&
           left->marker_generation == right->marker_generation &&
           left->storage_backend_generation ==
               right->storage_backend_generation &&
           left->storage_capacity_kb == right->storage_capacity_kb &&
           left->average_tile_bytes == right->average_tile_bytes &&
           left->cache_budget_mb == right->cache_budget_mb &&
           left->center_lat_e7 == right->center_lat_e7 &&
           left->center_lon_e7 == right->center_lon_e7 &&
           left->viewport_lat_e6 == right->viewport_lat_e6 &&
           left->viewport_lon_e6 == right->viewport_lon_e6 &&
           left->provider_max_zoom == right->provider_max_zoom &&
           left->viewport_valid == right->viewport_valid &&
           strcmp(left->source_id, right->source_id) == 0;
}

static void task_pause(void)
{
    (void)ulTaskNotifyTake(
        pdTRUE, pdMS_TO_TICKS(D1L_MAP_PREFETCH_POLL_MS));
}

static void task_pause_after_network_tile(void)
{
    (void)ulTaskNotifyTake(
        pdTRUE,
        pdMS_TO_TICKS(D1L_MAP_PREFETCH_INTER_TILE_PAUSE_MS));
}

static bool visible_map_active(void)
{
    return d1l_map_view_service_visible();
}

static bool prefetch_continue(void *context)
{
    const d1l_map_prefetch_continue_t *expected =
        (const d1l_map_prefetch_continue_t *)context;
    if (!expected || visible_map_active() ||
        d1l_node_store_marker_generation() !=
            expected->marker_generation) {
        return false;
    }
    d1l_connectivity_status_t connectivity = {0};
    d1l_connectivity_status(&connectivity);
    if (!connectivity.wifi_connected) {
        return false;
    }
    d1l_storage_status_t storage = {0};
    d1l_storage_status(&storage);
    if (!d1l_map_tile_store_sd_ready(&storage)) {
        return false;
    }
    d1l_map_tile_provider_t provider = {0};
    d1l_map_tile_provider_snapshot(&provider);
    return provider.configured &&
           provider.network_fetch_allowed &&
           provider.background_prefetch_permitted &&
           strcmp(provider.source_id, expected->source_id) == 0;
}

static void publish_waiting(const char *phase,
                            const char *message,
                            esp_err_t error)
{
    d1l_map_prefetch_status_t status = {
        .initialized = true,
        .last_error = error,
    };
    set_phase(&status, phase, message);
    publish_status(&status);
}

static size_t collect_points(void)
{
    const size_t marker_count = d1l_node_store_copy_markers(
        s_markers, D1L_NODE_SD_HISTORY_CAPACITY);
    for (size_t i = 0U; i < marker_count; ++i) {
        s_points[i] = (d1l_map_prefetch_point_t) {
            .lat_e6 = s_markers[i].lat_e6,
            .lon_e6 = s_markers[i].lon_e6,
        };
    }
    return marker_count;
}

static void note_stop_state(d1l_map_prefetch_status_t *status,
                            const d1l_map_prefetch_continue_t *continuation)
{
    if (!status || !continuation) {
        return;
    }
    status->running = false;
    if (visible_map_active()) {
        status->paused_for_visible_map = true;
        set_phase(status, "paused_visible",
                  "Background map download paused while Map is open");
    } else if (d1l_node_store_marker_generation() !=
               continuation->marker_generation) {
        set_phase(status, "replanning",
                  "Node locations changed; rebuilding the map area");
    } else {
        d1l_connectivity_status_t connectivity = {0};
        d1l_connectivity_status(&connectivity);
        status->wifi_connected = connectivity.wifi_connected;
        if (!connectivity.wifi_connected) {
            set_phase(status, "wifi_required",
                      "Background map download waits for Wi-Fi");
        } else {
            set_phase(status, "retrying",
                      "Background map download will retry");
        }
    }
}

static bool storage_has_download_room(
    const d1l_map_prefetch_plan_t *plan,
    uint64_t starting_free_bytes,
    uint64_t downloaded_bytes)
{
    if (!plan || starting_free_bytes <= downloaded_bytes) {
        return false;
    }
    const uint64_t remaining =
        starting_free_bytes - downloaded_bytes;
    return remaining >
        plan->reserve_bytes + D1L_MAP_TILE_DOWNLOAD_MAX_BYTES;
}

static void run_plan(const d1l_map_prefetch_plan_t *plan,
                     const d1l_map_tile_provider_t *provider,
                     const d1l_storage_status_t *starting_storage,
                     const d1l_map_prefetch_key_t *key,
                     d1l_map_prefetch_status_t *status)
{
    if (!plan || !provider || !starting_storage || !key || !status) {
        return;
    }
    const d1l_map_prefetch_continue_t continuation = {
        .marker_generation = key->marker_generation,
    };
    d1l_map_prefetch_continue_t mutable_continuation = continuation;
    snprintf(mutable_continuation.source_id,
             sizeof(mutable_continuation.source_id), "%s",
             provider->source_id);
    const uint64_t starting_free_bytes =
        (uint64_t)starting_storage->free_kb * 1024ULL;

    status->running = true;
    set_phase(status, "downloading",
              "Downloading the local node map in the background");
    publish_status(status);

    for (uint64_t index = 0U; index < plan->total_tiles; ++index) {
        if (!prefetch_continue(&mutable_continuation)) {
            note_stop_state(status, &mutable_continuation);
            publish_status(status);
            return;
        }
        uint8_t zoom = 0U;
        uint32_t x = 0U;
        uint32_t y = 0U;
        if (!d1l_map_prefetch_plan_tile_at(
                plan, index, &zoom, &x, &y)) {
            status->last_error = ESP_ERR_INVALID_STATE;
            ++status->failed_tiles;
            status->running = false;
            set_phase(status, "plan_error",
                      "The background map plan was invalid");
            publish_status(status);
            return;
        }

        d1l_storage_status_t storage = {0};
        d1l_storage_status(&storage);
        bool cached = false;
        esp_err_t ret = d1l_map_tile_store_cached(
            zoom, x, y, &storage, &cached);
        if (ret == ESP_OK && cached) {
            ++status->cached_tiles;
            ++status->visited_tiles;
            publish_status(status);
            taskYIELD();
            continue;
        }
        if (ret == ESP_ERR_NOT_FINISHED) {
            status->last_error = ret;
            status->running = false;
            set_phase(status, "storage_busy",
                      "Background map download is waiting for SD storage");
            publish_status(status);
            return;
        }
        if (ret != ESP_ERR_NOT_FOUND) {
            status->last_error = ret;
            ++status->failed_tiles;
            status->running = false;
            set_phase(status, "cache_check",
                      "The SD map cache could not be checked");
            publish_status(status);
            s_backoff_until_us = esp_timer_get_time() +
                (int64_t)D1L_MAP_PREFETCH_ERROR_BACKOFF_SEC * 1000000LL;
            return;
        }
        if (!storage_has_download_room(
                plan, starting_free_bytes,
                status->downloaded_bytes)) {
            status->storage_reserve_reached = true;
            status->running = false;
            set_phase(status, "storage_reserve",
                      "Map download stopped at the 8 GB card reserve");
            publish_status(status);
            return;
        }

        size_t downloaded_len = 0U;
        d1l_map_tile_download_result_t result = {0};
        ++s_network_requests_total;
        status->network_requests = s_network_requests_total;
        /*
         * Publish before the blocking HTTPS request. This is a monotonic count
         * of real provider fetch attempts, not planned or cache-hit tiles.
         */
        publish_status(status);
        ret = d1l_map_tile_store_fetch_background(
            zoom, x, y, &storage, true,
            s_tile_buffer, D1L_MAP_TILE_DOWNLOAD_MAX_BYTES,
            &downloaded_len, prefetch_continue,
            &mutable_continuation, &result);
        status->evicted_tiles += result.evicted_tiles;
        status->cache_used_bytes = result.cache_used_bytes;
        if (ret == ESP_ERR_NOT_FINISHED) {
            status->last_error = ret;
            status->running = false;
            set_phase(status, "storage_busy",
                      "Background map download is waiting for SD storage");
            memset(&result, 0, sizeof(result));
            publish_status(status);
            return;
        }
        if (result.status_code == 429 ||
            result.status_code == 503) {
            const uint32_t retry =
                result.retry_after_sec > 0U ?
                    result.retry_after_sec :
                    D1L_MAP_PREFETCH_DEFAULT_RATE_BACKOFF_SEC;
            status->last_error = ret;
            ++status->failed_tiles;
            ++status->visited_tiles;
            status->running = false;
            status->retry_after_sec = retry;
            set_phase(status, "rate_limited",
                      "Map provider asked the background download to wait");
            s_backoff_until_us = esp_timer_get_time() +
                (int64_t)retry * 1000000LL;
            memset(&result, 0, sizeof(result));
            publish_status(status);
            return;
        }
        if (ret == ESP_OK) {
            ++status->downloaded_tiles;
            ++status->visited_tiles;
            status->downloaded_bytes += downloaded_len;
            publish_status(status);
            memset(&result, 0, sizeof(result));
            task_pause_after_network_tile();
            continue;
        }
        if (result.cancelled ||
            !prefetch_continue(&mutable_continuation)) {
            memset(&result, 0, sizeof(result));
            note_stop_state(status, &mutable_continuation);
            publish_status(status);
            return;
        }
        status->last_error = ret;
        ++status->failed_tiles;
        ++status->visited_tiles;
        status->running = false;
        set_phase(status, result.step[0] ? result.step : "download_error",
                  "A background map tile could not be downloaded");
        s_backoff_until_us = esp_timer_get_time() +
            (int64_t)D1L_MAP_PREFETCH_ERROR_BACKOFF_SEC * 1000000LL;
        memset(&result, 0, sizeof(result));
        publish_status(status);
        return;
    }

    status->running = false;
    status->complete = true;
    set_phase(status, "ready",
              "Local node map is cached at the highest detail that fits");
    s_completed_key = *key;
    s_completed_key_valid = true;
    publish_status(status);
}

static __attribute__((noinline)) void run_prefetch_pass(void)
{
        d1l_settings_t settings = {0};
        if (d1l_settings_public_snapshot(&settings) != ESP_OK ||
            !settings.map_location_set) {
            publish_waiting(
                "location_required",
                 "Set the device location to enable background maps",
                 ESP_ERR_INVALID_STATE);
            task_pause();
            return;
        }

        d1l_storage_status_t storage = {0};
        d1l_storage_status(&storage);
        if (!d1l_map_tile_store_sd_ready(&storage)) {
            publish_waiting(
                "sd_required",
                 "Insert the prepared FAT32 DeskOS card for background maps",
                 ESP_ERR_NOT_SUPPORTED);
            task_pause();
            return;
        }
        const esp_err_t provider_ret =
            d1l_map_tile_provider_refresh(&storage);
        if (provider_ret != ESP_OK) {
            publish_waiting(
                "provider_config",
                 "The offline map provider file on the SD card is invalid",
                 provider_ret);
            task_pause();
            return;
        }
        d1l_map_tile_provider_t provider = {0};
        d1l_map_tile_provider_snapshot(&provider);
        if (!provider.configured ||
            !provider.offline_storage_permitted ||
            !provider.network_fetch_allowed ||
            !provider.background_prefetch_permitted) {
            d1l_map_prefetch_status_t status = {
                .initialized = true,
                .location_set = true,
                .sd_ready = true,
                .provider_configured = provider.configured,
                .background_prefetch_permitted =
                    provider.background_prefetch_permitted,
                .cache_budget_mb = provider.cache_budget_mb,
            };
            snprintf(status.source_id, sizeof(status.source_id), "%s",
                     provider.source_id);
            set_phase(
                &status, "provider_required",
                "Install an offline-authorized provider to enable background maps");
            publish_status(&status);
            task_pause();
            return;
        }

        d1l_connectivity_status_t connectivity = {0};
        d1l_connectivity_status(&connectivity);
        if (!connectivity.wifi_connected) {
            d1l_map_prefetch_status_t status = {
                .initialized = true,
                .location_set = true,
                .sd_ready = true,
                .provider_configured = true,
                .background_prefetch_permitted = true,
                .cache_budget_mb = provider.cache_budget_mb,
            };
            snprintf(status.source_id, sizeof(status.source_id), "%s",
                     provider.source_id);
            set_phase(&status, "wifi_required",
                      "Background map download waits for Wi-Fi");
            publish_status(&status);
            task_pause();
            return;
        }
        const int64_t now_us = esp_timer_get_time();
        if (now_us < s_backoff_until_us) {
            d1l_map_prefetch_status_t status = {
                .initialized = true,
                .eligible = true,
                .location_set = true,
                .wifi_connected = true,
                .sd_ready = true,
                .provider_configured = true,
                .background_prefetch_permitted = true,
                .cache_budget_mb = provider.cache_budget_mb,
                .retry_after_sec = (uint32_t)(
                    (s_backoff_until_us - now_us + 999999LL) /
                    1000000LL),
            };
            snprintf(status.source_id, sizeof(status.source_id), "%s",
                     provider.source_id);
            set_phase(&status, "backoff",
                      "Background map download is waiting before retry");
            publish_status(&status);
            task_pause();
            return;
        }

        const uint32_t marker_generation =
            d1l_node_store_marker_generation();
        const size_t marker_count = collect_points();
        d1l_map_view_status_t last_view = {0};
        d1l_map_view_service_status(&last_view);
        d1l_map_prefetch_point_t viewport = {0};
        const bool viewport_valid =
            last_view.generation != 0U &&
            last_view.width > 0U && last_view.height > 0U &&
            last_view.lat_e7 >= -900000000 &&
            last_view.lat_e7 <= 900000000 &&
            last_view.lon_e7 >= -1800000000LL &&
            last_view.lon_e7 <= 1800000000LL;
        if (viewport_valid) {
            viewport.lat_e6 = last_view.lat_e7 / 10;
            viewport.lon_e6 = last_view.lon_e7 / 10;
        }
        d1l_map_prefetch_plan_t plan = {0};
        if (!d1l_map_prefetch_plan_build_with_viewport(
                settings.map_lat_e7, settings.map_lon_e7,
                s_points, marker_count,
                viewport_valid ? &viewport : NULL,
                storage.capacity_kb,
                (uint64_t)provider.cache_budget_mb * 1024ULL * 1024ULL,
                provider.average_tile_bytes, provider.max_zoom,
                &plan)) {
            publish_waiting(
                "capacity_required",
                 "The SD card cannot hold the minimum local map area",
                 ESP_ERR_INVALID_SIZE);
            task_pause();
            return;
        }
        d1l_map_prefetch_key_t key = {
            .marker_generation = marker_generation,
            .storage_capacity_kb = storage.capacity_kb,
            .average_tile_bytes = provider.average_tile_bytes,
            .cache_budget_mb = provider.cache_budget_mb,
            .center_lat_e7 = settings.map_lat_e7,
            .center_lon_e7 = settings.map_lon_e7,
            .viewport_lat_e6 = viewport.lat_e6,
            .viewport_lon_e6 = viewport.lon_e6,
            .provider_max_zoom = provider.max_zoom,
            .viewport_valid = viewport_valid,
        };
        snprintf(key.source_id, sizeof(key.source_id), "%s",
                 provider.source_id);
        d1l_retained_blob_store_backend_state_t backend = {0};
        if (d1l_retained_blob_store_backend_state(
                D1L_RETAINED_BLOB_STORE_ROUTES,
                &backend)) {
            key.storage_backend_generation = backend.generation;
        }
        if (s_completed_key_valid &&
            key_equal(&key, &s_completed_key)) {
            d1l_map_prefetch_status_t status = {0};
            d1l_map_prefetch_service_status(&status);
            status.initialized = true;
            status.eligible = true;
            status.complete = true;
            status.location_set = true;
            status.wifi_connected = true;
            status.sd_ready = true;
            set_phase(
                &status, "ready",
                "Local node map is cached at the highest detail that fits");
            publish_status(&status);
            task_pause();
            return;
        }

        d1l_map_prefetch_status_t status = {
            .initialized = true,
            .eligible = true,
            .location_set = true,
            .wifi_connected = true,
            .sd_ready = true,
            .provider_configured = true,
            .background_prefetch_permitted = true,
            .cache_budget_mb = provider.cache_budget_mb,
            .selected_max_zoom = plan.max_zoom,
            .marker_generation = marker_generation,
            .storage_capacity_kb = storage.capacity_kb,
            .storage_free_kb = storage.free_kb,
            .nodes_seen = plan.node_count_seen,
            .nodes_included = plan.node_count_included,
            .nodes_outside_radius =
                plan.node_count_outside_radius,
            .total_tiles = plan.total_tiles,
            .estimated_bytes = plan.estimated_bytes,
            .allocation_bytes = plan.allocation_bytes,
        };
        snprintf(status.source_id, sizeof(status.source_id), "%s",
                 provider.source_id);
        if (!storage_has_download_room(
                &plan, (uint64_t)storage.free_kb * 1024ULL, 0U)) {
            status.storage_reserve_reached = true;
            set_phase(
                &status, "storage_reserve",
                "Background map download is held at the 8 GB card reserve");
            publish_status(&status);
            task_pause();
            return;
        }
        run_plan(&plan, &provider, &storage, &key, &status);
        task_pause();
}

static __attribute__((noinline)) void publish_visible_pause(void)
{
    d1l_map_prefetch_status_t status = {0};
    d1l_map_prefetch_service_status(&status);
    status.initialized = true;
    status.running = false;
    status.eligible = true;
    status.paused_for_visible_map = true;
    set_phase(&status, "paused_visible",
              "Background map download paused while Map is open");
    publish_status(&status);
}

static void prefetch_worker(void *context)
{
    (void)context;
    for (;;) {
        if (!visible_map_active()) {
            run_prefetch_pass();
            continue;
        }

        publish_visible_pause();
        d1l_map_view_service_run_pending();
        task_pause();
    }
}

esp_err_t d1l_map_prefetch_service_init(void)
{
    bool claimed_start = false;
    portENTER_CRITICAL(&s_status_lock);
    const bool already_started = s_worker != NULL;
    if (!already_started && !s_starting) {
        s_starting = true;
        claimed_start = true;
    }
    portEXIT_CRITICAL(&s_status_lock);
    if (already_started) {
        return ESP_OK;
    }
    if (!claimed_start) {
        return ESP_ERR_INVALID_STATE;
    }

    s_markers = heap_caps_calloc(
        D1L_NODE_SD_HISTORY_CAPACITY, sizeof(*s_markers),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_points = heap_caps_calloc(
        D1L_NODE_SD_HISTORY_CAPACITY, sizeof(*s_points),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tile_buffer = heap_caps_malloc(
        D1L_MAP_TILE_DOWNLOAD_MAX_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_markers || !s_points || !s_tile_buffer) {
        heap_caps_free(s_markers);
        heap_caps_free(s_points);
        heap_caps_free(s_tile_buffer);
        s_markers = NULL;
        s_points = NULL;
        s_tile_buffer = NULL;
        portENTER_CRITICAL(&s_status_lock);
        s_starting = false;
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_ERR_NO_MEM;
    }

    SemaphoreHandle_t wake_lock = xSemaphoreCreateMutex();
    if (!wake_lock) {
        heap_caps_free(s_markers);
        heap_caps_free(s_points);
        heap_caps_free(s_tile_buffer);
        s_markers = NULL;
        s_points = NULL;
        s_tile_buffer = NULL;
        portENTER_CRITICAL(&s_status_lock);
        s_starting = false;
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_status_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.worker_stack_bytes =
        D1L_MAP_PREFETCH_WORKER_STACK_BYTES;
    snprintf(s_status.phase, sizeof(s_status.phase), "%s", "starting");
    snprintf(s_status.message, sizeof(s_status.message), "%s",
             "Checking background map requirements");
    portEXIT_CRITICAL(&s_status_lock);

    TaskHandle_t worker = NULL;
    if (xTaskCreate(
            prefetch_worker, "map_prefetch",
            D1L_MAP_PREFETCH_WORKER_STACK_BYTES, NULL,
            D1L_MAP_PREFETCH_WORKER_PRIORITY,
            &worker) != pdPASS) {
        heap_caps_free(s_markers);
        heap_caps_free(s_points);
        heap_caps_free(s_tile_buffer);
        s_markers = NULL;
        s_points = NULL;
        s_tile_buffer = NULL;
        vSemaphoreDelete(wake_lock);
        portENTER_CRITICAL(&s_status_lock);
        s_starting = false;
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_status_lock);
    s_wake_lock = wake_lock;
    s_worker = worker;
    s_starting = false;
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}

esp_err_t d1l_map_prefetch_service_wake(void)
{
    portENTER_CRITICAL(&s_status_lock);
    TaskHandle_t worker = s_worker;
    SemaphoreHandle_t wake_lock = s_wake_lock;
    portEXIT_CRITICAL(&s_status_lock);
    if (!worker || !wake_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * Serialize the visibility snapshot with priority application. A matching
     * wake follows every visible-lease transition, so a racing transition
     * waits here and then applies the latest state instead of losing a
     * foreground promotion to a stale demotion.
     */
    if (xSemaphoreTake(wake_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const bool visible = visible_map_active();
    vTaskPrioritySet(
        worker,
        visible ?
            D1L_MAP_VISIBLE_WORKER_PRIORITY :
            D1L_MAP_PREFETCH_WORKER_PRIORITY);
    xTaskNotifyGive(worker);
    xSemaphoreGive(wake_lock);
    if (visible) {
        /*
         * Socket shutdown may wake the worker immediately. Keep it outside
         * both the visibility and wake locks so the worker can unwind without
         * a lock-order dependency.
         */
        d1l_map_tile_store_cancel_background_fetch();
    }
    return ESP_OK;
}
