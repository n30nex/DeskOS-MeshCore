#include "map_view_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "comms/connectivity_manager.h"
#include "map/map_prefetch_service.h"
#include "map/map_png_decoder.h"
#include "map/map_tile_provider.h"
#include "storage/map_tile_store.h"
#include "storage/storage_status.h"

#define D1L_MAP_SD_POLL_MS 500U
#define D1L_MAP_WIFI_POLL_MS 500U
#define D1L_MAP_TILE_GAP_MS 100U
#define D1L_MAP_DEFAULT_RETRY_AFTER_SEC 300U

typedef struct {
    SemaphoreHandle_t lock;
    d1l_map_view_status_t status;
    d1l_map_tile_plan_t plan;
    uint16_t *frames[2];
    uint8_t frame_readers[2];
    uint8_t front_slot;
    uint8_t *compressed;
    uint16_t *decoded_tile;
    int64_t backoff_until_us;
    bool force_reload_next_acquire;
} map_service_t;

static map_service_t s_map;

static void set_provider_locked(const d1l_map_tile_provider_t *provider)
{
    if (!provider) {
        return;
    }
    s_map.status.provider_configured = provider->configured;
    s_map.status.background_prefetch_permitted =
        provider->background_prefetch_permitted;
    s_map.status.provider_max_zoom = provider->max_zoom;
    snprintf(s_map.status.source_id, sizeof(s_map.status.source_id), "%s",
             provider->source_id);
    snprintf(s_map.status.attribution, sizeof(s_map.status.attribution), "%s",
             provider->attribution);
}

static void set_message_locked(const char *phase, const char *message)
{
    snprintf(s_map.status.phase, sizeof(s_map.status.phase), "%s",
             phase ? phase : "unknown");
    snprintf(s_map.status.message, sizeof(s_map.status.message), "%s",
             message ? message : "");
}

static bool storage_text_equals(const char *value, const char *expected)
{
    return value && expected && strcmp(value, expected) == 0;
}

static void set_storage_wait_message_locked(const d1l_storage_status_t *storage)
{
    if (!storage) {
        set_message_locked("sd_cache_required", "Waiting for the SD cache");
        return;
    }
    const bool needs_attention = storage->retained_sd_degraded ||
        storage_text_equals(storage->sd_state, "error") ||
        storage_text_equals(storage->sd_state, "bridge_reported") ||
        storage_text_equals(storage->setup_action,
                            "inspect_rp2040_sd_cmd0_firmware_path") ||
        storage_text_equals(storage->setup_action,
                            "inspect_rp2040_sd_mount_error_firmware_path");
    if (needs_attention) {
        set_message_locked("sd_attention", "Check the SD card");
    } else if (storage_text_equals(storage->setup_action, "insert_card")) {
        set_message_locked("sd_card_required", "Insert a FAT32 SD card");
    } else if (storage->sd_needs_fat32) {
        set_message_locked("sd_fat32_required", "A FAT32 SD card is required");
    } else if (storage_text_equals(storage->setup_action,
                                   "wait_for_storage_reconnect")) {
        set_message_locked("sd_reconnecting", "Storage is reconnecting");
    } else {
        set_message_locked("sd_cache_required", "Waiting for the SD cache");
    }
}

static bool generation_visible_locked(uint32_t generation)
{
    return s_map.status.visible && s_map.status.generation == generation;
}

static bool completed_frame_locked(void)
{
    return s_map.status.frame_ready &&
           s_map.status.frame_revision > 0U &&
           s_map.status.planned_tiles > 0U &&
           s_map.status.attempted_tiles == s_map.status.planned_tiles &&
           s_map.status.rendered_tiles == s_map.status.planned_tiles &&
           s_map.status.failed_tiles == 0U;
}

static bool generation_continue(void *context)
{
    if (!context || !s_map.lock) {
        return false;
    }
    const uint32_t generation = *(const uint32_t *)context;
    bool allowed = false;
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        allowed = generation_visible_locked(generation);
        xSemaphoreGive(s_map.lock);
    }
    return allowed;
}

static bool wait_for_frame_slot(uint8_t slot, uint32_t generation)
{
    while (generation_continue(&generation)) {
        bool free_slot = false;
        if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            free_slot = s_map.frame_readers[slot] == 0U;
            xSemaphoreGive(s_map.lock);
        }
        if (free_slot) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static bool wait_for_sd_cache(uint32_t generation,
                              d1l_storage_status_t *out_storage)
{
    if (!out_storage) {
        return false;
    }
    while (generation_continue(&generation)) {
        d1l_storage_status_t storage = {0};
        d1l_storage_status(&storage);
        const bool ready = d1l_map_tile_store_sd_ready(&storage);
        if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_map.status.generation == generation) {
                s_map.status.sd_cache_ready = ready;
                if (ready) {
                    set_message_locked("loading_cache", "Loading current map view");
                } else {
                    set_storage_wait_message_locked(&storage);
                }
            }
            xSemaphoreGive(s_map.lock);
        }
        if (ready) {
            *out_storage = storage;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(D1L_MAP_SD_POLL_MS));
    }
    return false;
}

static void fill_placeholder(uint16_t *pixels, uint16_t width, uint16_t height)
{
    if (!pixels) {
        return;
    }
    for (uint16_t y = 0; y < height; ++y) {
        for (uint16_t x = 0; x < width; ++x) {
            const bool alternate = (((x / 64U) + (y / 64U)) & 1U) != 0U;
            pixels[(size_t)y * width + x] = alternate ? 0x0862U : 0x10C4U;
        }
    }
}

static void draw_tile(uint16_t *frame,
                      uint16_t width,
                      uint16_t height,
                      const d1l_map_tile_placement_t *placement,
                      const uint16_t *tile)
{
    if (!frame || !placement || !tile) {
        return;
    }
    for (int source_y = 0; source_y < (int)D1L_MAP_TILE_PIXEL_SIZE; ++source_y) {
        const int dest_y = (int)placement->screen_y + source_y;
        if (dest_y < 0 || dest_y >= height) {
            continue;
        }
        int source_x = 0;
        int dest_x = placement->screen_x;
        int copy_width = D1L_MAP_TILE_PIXEL_SIZE;
        if (dest_x < 0) {
            source_x = -dest_x;
            copy_width -= source_x;
            dest_x = 0;
        }
        if (dest_x + copy_width > width) {
            copy_width = width - dest_x;
        }
        if (copy_width <= 0) {
            continue;
        }
        memcpy(&frame[(size_t)dest_y * width + (size_t)dest_x],
               &tile[(size_t)source_y * D1L_MAP_TILE_PIXEL_SIZE + (size_t)source_x],
               (size_t)copy_width * sizeof(uint16_t));
    }
}

static bool publish_initial_frame(const d1l_map_tile_plan_t *plan, uint32_t generation)
{
    uint8_t work_slot = 0U;
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    work_slot = (uint8_t)(1U - s_map.front_slot);
    xSemaphoreGive(s_map.lock);
    if (!wait_for_frame_slot(work_slot, generation)) {
        return false;
    }
    fill_placeholder(s_map.frames[work_slot], plan->width, plan->height);
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    if (!generation_visible_locked(generation)) {
        xSemaphoreGive(s_map.lock);
        return false;
    }
    s_map.front_slot = work_slot;
    xSemaphoreGive(s_map.lock);
    return true;
}

static bool publish_tile_frame(const d1l_map_tile_plan_t *plan,
                               const d1l_map_tile_placement_t *placement,
                               uint32_t generation)
{
    uint8_t front = 0U;
    uint8_t work = 0U;
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    front = s_map.front_slot;
    work = (uint8_t)(1U - front);
    xSemaphoreGive(s_map.lock);
    if (!wait_for_frame_slot(work, generation)) {
        return false;
    }
    const size_t frame_bytes = (size_t)plan->width * plan->height * sizeof(uint16_t);
    memcpy(s_map.frames[work], s_map.frames[front], frame_bytes);
    draw_tile(s_map.frames[work], plan->width, plan->height, placement, s_map.decoded_tile);

    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    if (!generation_visible_locked(generation)) {
        xSemaphoreGive(s_map.lock);
        return false;
    }
    s_map.front_slot = work;
    s_map.status.frame_ready = true;
    ++s_map.status.frame_revision;
    ++s_map.status.rendered_tiles;
    xSemaphoreGive(s_map.lock);
    return true;
}

static void note_failure(uint32_t generation, const char *phase, const char *message)
{
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_map.status.generation == generation) {
            if (s_map.status.failed_tiles < UINT8_MAX) {
                ++s_map.status.failed_tiles;
            }
            set_message_locked(phase, message);
        }
        xSemaphoreGive(s_map.lock);
    }
}

static void note_tile_failure(
    uint32_t generation,
    const d1l_map_tile_download_result_t *result)
{
    if (!result) {
        return;
    }
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_map.status.generation == generation) {
            snprintf(
                s_map.status.last_failure_step,
                sizeof(s_map.status.last_failure_step), "%s",
                result->step[0] ? result->step : "unknown");
            snprintf(
                s_map.status.last_failure_detail_step,
                sizeof(s_map.status.last_failure_detail_step), "%s",
                result->persistence_step);
            s_map.status.last_failure_error = result->last_error;
            s_map.status.last_failure_http_status = result->status_code;
            s_map.status.last_failure_retry_after_sec =
                result->retry_after_sec;
            s_map.status.last_failure_bytes =
                result->bytes > UINT32_MAX ?
                    UINT32_MAX : (uint32_t)result->bytes;
            s_map.status.last_failure_content_type_valid =
                result->content_type_valid;
            s_map.status.last_failure_png_valid = result->png_valid;
            s_map.status.last_failure_checksum_verified =
                result->checksum_verified;
            s_map.status.last_failure_cache_intent_recorded =
                result->cache_intent_recorded;
            s_map.status.last_failure_zoom = result->z;
            s_map.status.last_failure_x = result->x;
            s_map.status.last_failure_y = result->y;
            s_map.status.last_failure_file_ok = result->file.ok;
            s_map.status.last_failure_file_response_truncated =
                result->file.response_truncated;
            s_map.status.last_failure_file_cancelled =
                result->file.cancelled;
            s_map.status.last_failure_file_error =
                result->file.last_error;
            s_map.status.last_failure_file_size = result->file.size;
            s_map.status.last_failure_file_offset = result->file.offset;
            s_map.status.last_failure_file_length = result->file.length;
            snprintf(
                s_map.status.last_failure_file_op,
                sizeof(s_map.status.last_failure_file_op), "%s",
                result->file.op);
            snprintf(
                s_map.status.last_failure_file_err,
                sizeof(s_map.status.last_failure_file_err), "%s",
                result->file.err);
            snprintf(
                s_map.status.last_failure_file_note,
                sizeof(s_map.status.last_failure_file_note), "%s",
                result->file.note);
        }
        xSemaphoreGive(s_map.lock);
    }
}

static void note_worker_stack(void)
{
    const uint32_t free_bytes =
        (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_map.status.worker_stack_bytes =
            D1L_MAP_SHARED_WORKER_STACK_BYTES;
        s_map.status.worker_stack_free_bytes = free_bytes;
        xSemaphoreGive(s_map.lock);
    }
}

static bool wait_for_wifi(uint32_t generation)
{
    while (generation_continue(&generation)) {
        d1l_connectivity_status_t connectivity = {0};
        d1l_connectivity_status(&connectivity);
        if (connectivity.wifi_connected) {
            if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (s_map.status.generation == generation) {
                    s_map.status.wifi_connected = true;
                }
                xSemaphoreGive(s_map.lock);
            }
            return true;
        }
        if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_map.status.generation == generation) {
                s_map.status.wifi_connected = false;
                set_message_locked("wifi_required", "Enable Wi-Fi to load missing map tiles");
            }
            xSemaphoreGive(s_map.lock);
        }
        vTaskDelay(pdMS_TO_TICKS(D1L_MAP_WIFI_POLL_MS));
    }
    return false;
}

static bool rate_limit_active(uint32_t generation)
{
    const int64_t now_us = esp_timer_get_time();
    bool active = false;
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        active = now_us < s_map.backoff_until_us;
        if (active && s_map.status.generation == generation) {
            const int64_t remaining_us = s_map.backoff_until_us - now_us;
            s_map.status.rate_limited = true;
            s_map.status.retry_after_sec = (uint32_t)((remaining_us + 999999LL) / 1000000LL);
            set_message_locked("rate_limited", "Map service asked us to wait");
        }
        xSemaphoreGive(s_map.lock);
    }
    return active;
}

static void run_generation(const d1l_map_tile_plan_t *plan, uint32_t generation)
{
    /*
     * A visible generation may be retried after a transient tile error. These
     * counters describe one bounded pass, not the lifetime of the generation;
     * reset them atomically before any provider, storage, network, or frame I/O.
     * Keep the generation, plan, last published frame/revision, and retained
     * failure detail intact until this pass proves a complete replacement.
     */
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (!generation_visible_locked(generation)) {
        xSemaphoreGive(s_map.lock);
        return;
    }
    /*
     * A Retry-After window pauses this generation; it does not consume one of
     * its bounded tile passes. The worker is polled while Map remains visible,
     * so counting those polls would exhaust the retry budget before the
     * provider permits another request.
     */
    const int64_t now_us = esp_timer_get_time();
    if (now_us < s_map.backoff_until_us) {
        const int64_t remaining_us = s_map.backoff_until_us - now_us;
        s_map.status.worker_running = false;
        s_map.status.rate_limited = true;
        s_map.status.retry_after_sec =
            (uint32_t)((remaining_us + 999999LL) / 1000000LL);
        set_message_locked("rate_limited", "Map service asked us to wait");
        xSemaphoreGive(s_map.lock);
        return;
    }
    if (s_map.status.pass_attempts >=
        D1L_MAP_VIEW_MAX_GENERATION_PASSES) {
        xSemaphoreGive(s_map.lock);
        return;
    }
    const bool publish_placeholder = !s_map.status.frame_ready;
    ++s_map.status.pass_attempts;
    s_map.status.worker_running = true;
    s_map.status.attempted_tiles = 0U;
    s_map.status.cache_hits = 0U;
    s_map.status.network_requests = 0U;
    s_map.status.downloaded_tiles = 0U;
    s_map.status.rendered_tiles = 0U;
    s_map.status.failed_tiles = 0U;
    s_map.status.decode_total_us = 0U;
    s_map.status.decode_max_us = 0U;
    s_map.status.decode_samples = 0U;
    s_map.status.rate_limited = false;
    s_map.status.retry_after_sec = 0U;
    set_message_locked("preparing", "Preparing current map view");
    xSemaphoreGive(s_map.lock);

    d1l_storage_status_t storage = {0};
    d1l_storage_status(&storage);
    const bool sd_ready = d1l_map_tile_store_sd_ready(&storage);
    d1l_connectivity_status_t initial_connectivity = {0};
    d1l_connectivity_status(&initial_connectivity);
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (generation_visible_locked(generation)) {
            s_map.status.sd_cache_ready = sd_ready;
            s_map.status.wifi_connected = initial_connectivity.wifi_connected;
            if (sd_ready) {
                set_message_locked("loading_cache", "Loading current map view");
            } else {
                set_storage_wait_message_locked(&storage);
            }
        }
        xSemaphoreGive(s_map.lock);
    }
    if (!sd_ready && !wait_for_sd_cache(generation, &storage)) {
        return;
    }
    const esp_err_t provider_ret =
        d1l_map_tile_provider_refresh(&storage);
    if (provider_ret != ESP_OK) {
        note_failure(
            generation, "provider_config",
            "The SD map provider configuration is invalid");
        return;
    }
    d1l_map_tile_provider_t provider = {0};
    d1l_map_tile_provider_snapshot(&provider);
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_map.status.generation == generation) {
            set_provider_locked(&provider);
        }
        xSemaphoreGive(s_map.lock);
    }
    if (publish_placeholder &&
        !publish_initial_frame(plan, generation)) {
        return;
    }

    for (uint8_t i = 0; i < plan->count && generation_continue(&generation); ++i) {
        if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_map.status.generation == generation) {
                ++s_map.status.attempted_tiles;
                set_message_locked("loading_cache", "Loading current map view");
            }
            xSemaphoreGive(s_map.lock);
        }

        size_t compressed_len = 0U;
        bool downloaded = false;
        d1l_map_tile_download_result_t tile_result = {0};
        esp_err_t ret = d1l_map_tile_store_read(
            plan->tiles[i].zoom, plan->tiles[i].x, plan->tiles[i].y, &storage,
            s_map.compressed, D1L_MAP_TILE_DOWNLOAD_MAX_BYTES, &compressed_len,
            generation_continue, &generation, &tile_result);
        if (ret == ESP_OK) {
            if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (s_map.status.generation == generation) {
                    ++s_map.status.cache_hits;
                }
                xSemaphoreGive(s_map.lock);
            }
        } else {
            if (tile_result.cancelled || !generation_continue(&generation)) {
                break;
            }
            if (!tile_result.provider_allowed) {
                note_failure(
                    generation, "provider_preloaded_only",
                    "This SD map source has no network download URL");
                continue;
            }
            if (rate_limit_active(generation) || !wait_for_wifi(generation)) {
                break;
            }
            if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (s_map.status.generation == generation) {
                    ++s_map.status.network_requests;
                    set_message_locked("downloading", "Downloading current map view");
                }
                xSemaphoreGive(s_map.lock);
            }
            d1l_storage_status(&storage);
            ret = d1l_map_tile_store_fetch(
                plan->tiles[i].zoom, plan->tiles[i].x, plan->tiles[i].y, &storage,
                true, s_map.compressed, D1L_MAP_TILE_DOWNLOAD_MAX_BYTES,
                &compressed_len, generation_continue, &generation, &tile_result);
            note_worker_stack();
            if (ret == ESP_OK) {
                downloaded = true;
                if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (s_map.status.generation == generation) {
                        ++s_map.status.downloaded_tiles;
                    }
                    xSemaphoreGive(s_map.lock);
                }
            } else {
                note_tile_failure(generation, &tile_result);
                if (tile_result.status_code == 429 || tile_result.status_code == 503) {
                    const uint32_t retry = tile_result.retry_after_sec > 0U ?
                                           tile_result.retry_after_sec :
                                           D1L_MAP_DEFAULT_RETRY_AFTER_SEC;
                    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                        s_map.backoff_until_us = esp_timer_get_time() +
                                                 ((int64_t)retry * 1000000LL);
                        if (s_map.status.generation == generation) {
                            s_map.status.rate_limited = true;
                            s_map.status.retry_after_sec = retry;
                            set_message_locked("rate_limited", "Map service asked us to wait");
                        }
                        xSemaphoreGive(s_map.lock);
                    }
                    break;
                }
                if (tile_result.cancelled || !generation_continue(&generation)) {
                    break;
                }
                note_failure(generation, tile_result.step, "A map tile could not be loaded");
                if (strcmp(tile_result.step, "time_sync") == 0) {
                    /* Secure HTTPS is fail-closed until SNTP establishes a
                     * certificate-valid wall clock.  Repeating the same wait
                     * for every tile would only delay cancellation. */
                    break;
                }
                /* A concrete HTTP error applies to the fixed source, not one
                 * coordinate.  Stop this generation after the first response
                 * instead of repeating a policy/server failure for every tile.
                 * Transport failures have no HTTP status and may still allow a
                 * later cached/current-view coordinate to succeed. */
                if (tile_result.status_code != 0 && tile_result.status_code != 200) {
                    break;
                }
                continue;
            }
        }

        if (!generation_continue(&generation)) {
            break;
        }
        const int64_t decode_started_us = esp_timer_get_time();
        ret = d1l_map_png_decode_rgb565(s_map.compressed, compressed_len,
                                        s_map.decoded_tile,
                                        D1L_MAP_DECODED_TILE_PIXELS);
        const int64_t decode_elapsed_us = esp_timer_get_time() - decode_started_us;
        if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_map.status.generation == generation) {
                const uint32_t elapsed_us = decode_elapsed_us <= 0 ? 0U :
                    (decode_elapsed_us > UINT32_MAX ? UINT32_MAX :
                     (uint32_t)decode_elapsed_us);
                if (UINT32_MAX - s_map.status.decode_total_us < elapsed_us) {
                    s_map.status.decode_total_us = UINT32_MAX;
                } else {
                    s_map.status.decode_total_us += elapsed_us;
                }
                if (elapsed_us > s_map.status.decode_max_us) {
                    s_map.status.decode_max_us = elapsed_us;
                }
                if (s_map.status.decode_samples < UINT8_MAX) {
                    ++s_map.status.decode_samples;
                }
            }
            xSemaphoreGive(s_map.lock);
        }
        if (ret != ESP_OK) {
            note_failure(generation, "decode", "A cached map tile was invalid");
            continue;
        }
        if (!publish_tile_frame(plan, &plan->tiles[i], generation)) {
            break;
        }
        if (downloaded) {
            vTaskDelay(pdMS_TO_TICKS(D1L_MAP_TILE_GAP_MS));
        } else {
            taskYIELD();
        }
    }

    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_map.status.generation == generation && s_map.status.visible) {
            if (s_map.status.rendered_tiles == s_map.status.planned_tiles) {
                set_message_locked("ready", "Map ready");
            } else if (!s_map.status.rate_limited && s_map.status.failed_tiles > 0U) {
                set_message_locked(
                    "partial",
                    s_map.status.pass_attempts >=
                            D1L_MAP_VIEW_MAX_GENERATION_PASSES ?
                        "Map partially loaded; reopen Map to retry" :
                        "Map partially loaded; retrying");
            }
        }
        xSemaphoreGive(s_map.lock);
    }
}

void d1l_map_view_service_run_pending(void)
{
    if (!s_map.lock) {
        return;
    }
    while (true) {
        d1l_map_tile_plan_t plan = {0};
        uint32_t generation = 0U;
        if (xSemaphoreTake(s_map.lock, portMAX_DELAY) == pdTRUE) {
            if (!s_map.status.visible) {
                s_map.status.worker_running = false;
                xSemaphoreGive(s_map.lock);
                break;
            }
            if (completed_frame_locked()) {
                s_map.status.worker_running = false;
                set_message_locked("ready", "Map ready");
                xSemaphoreGive(s_map.lock);
                break;
            }
            if (s_map.status.pass_attempts >=
                D1L_MAP_VIEW_MAX_GENERATION_PASSES) {
                s_map.status.worker_running = false;
                xSemaphoreGive(s_map.lock);
                break;
            }
            plan = s_map.plan;
            generation = s_map.status.generation;
            xSemaphoreGive(s_map.lock);
        }

        /* The generation snapshot above is authoritative. Drain the wake that
         * selected it so a superseded request cannot replay the same plan. */
        (void)ulTaskNotifyTake(pdTRUE, 0U);

        run_generation(&plan, generation);
        if (xSemaphoreTake(s_map.lock, portMAX_DELAY) == pdTRUE) {
            if (s_map.status.generation == generation) {
                s_map.status.worker_running = false;
            }
            const bool rerun = s_map.status.visible &&
                               s_map.status.generation != generation;
            xSemaphoreGive(s_map.lock);
            if (!rerun) {
                break;
            }
        }
    }
}

esp_err_t d1l_map_view_service_init(void)
{
    if (s_map.status.initialized) {
        return ESP_OK;
    }
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (!lock) {
        return ESP_ERR_NO_MEM;
    }
    const size_t frame_bytes = (size_t)D1L_MAP_VIEW_MAX_WIDTH *
                               D1L_MAP_VIEW_MAX_HEIGHT * sizeof(uint16_t);
    uint16_t *frame0 = heap_caps_malloc(
        frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *frame1 = heap_caps_malloc(
        frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *compressed = heap_caps_malloc(
        D1L_MAP_TILE_DOWNLOAD_MAX_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *decoded_tile = heap_caps_malloc(
        D1L_MAP_DECODED_TILE_PIXELS * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame0 || !frame1 || !compressed || !decoded_tile) {
        heap_caps_free(frame0);
        heap_caps_free(frame1);
        heap_caps_free(compressed);
        heap_caps_free(decoded_tile);
        vSemaphoreDelete(lock);
        return ESP_ERR_NO_MEM;
    }
    /*
     * Publish only a complete service. The prefetch task may ask whether Map
     * is visible while these large allocations run; it must see either the
     * zero-initialized service or this fully owned, never-deleted mutex.
     */
    memset(&s_map, 0, sizeof(s_map));
    s_map.frames[0] = frame0;
    s_map.frames[1] = frame1;
    s_map.compressed = compressed;
    s_map.decoded_tile = decoded_tile;
    s_map.status.current_view_only = true;
    s_map.status.public_rf_tx = false;
    s_map.status.formats_sd = false;
    s_map.status.worker_stack_bytes =
        D1L_MAP_SHARED_WORKER_STACK_BYTES;
    d1l_map_tile_provider_t provider = {0};
    d1l_map_tile_provider_builtin(&provider);
    set_provider_locked(&provider);
    set_message_locked("idle", "Open Map to load the current view");
    s_map.status.initialized = true;
    /* Publish the stable readiness token only after the service is complete. */
    s_map.lock = lock;
    return ESP_OK;
}

esp_err_t d1l_map_view_service_acquire_visible(int32_t lat_e7,
                                               int32_t lon_e7,
                                               uint8_t zoom,
                                               uint16_t width,
                                               uint16_t height,
                                               uint32_t *out_generation)
{
    if (!out_generation || zoom < D1L_MAP_VIEW_MIN_ZOOM ||
        zoom > D1L_MAP_VIEW_MAX_ZOOM) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = d1l_map_view_service_init();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_map_tile_plan_t plan = {0};
    if (!d1l_map_math_plan_current_view(lat_e7, lon_e7, zoom, width, height, &plan)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const bool same_plan = s_map.plan.lat_e7 == plan.lat_e7 &&
                           s_map.plan.lon_e7 == plan.lon_e7 &&
                           s_map.plan.zoom == plan.zoom &&
                           s_map.plan.width == plan.width &&
                           s_map.plan.height == plan.height;
    const bool force_reload = s_map.force_reload_next_acquire;
    s_map.force_reload_next_acquire = false;
    if (!force_reload && same_plan &&
        (s_map.status.visible || completed_frame_locked())) {
        s_map.status.visible = true;
        const uint32_t generation = s_map.status.generation;
        if (completed_frame_locked()) {
            s_map.status.worker_running = false;
            set_message_locked("ready", "Map ready");
        }
        xSemaphoreGive(s_map.lock);
        ret = d1l_map_prefetch_service_wake();
        if (ret != ESP_OK) {
            if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
                if (s_map.status.generation == generation) {
                    s_map.status.visible = false;
                    set_message_locked("worker_unavailable",
                                       "Map worker is unavailable");
                }
                xSemaphoreGive(s_map.lock);
            }
            return ret;
        }
        *out_generation = generation;
        return ESP_OK;
    }
    uint32_t generation = s_map.status.generation + 1U;
    if (generation == 0U) {
        generation = 1U;
    }
    s_map.plan = plan;
    memset(&s_map.status, 0, sizeof(s_map.status));
    s_map.status.initialized = true;
    s_map.status.visible = true;
    s_map.status.current_view_only = true;
    s_map.status.public_rf_tx = false;
    s_map.status.formats_sd = false;
    d1l_map_tile_provider_t provider = {0};
    d1l_map_tile_provider_snapshot(&provider);
    set_provider_locked(&provider);
    s_map.status.generation = generation;
    s_map.status.lat_e7 = plan.lat_e7;
    s_map.status.lon_e7 = plan.lon_e7;
    s_map.status.zoom = plan.zoom;
    s_map.status.width = plan.width;
    s_map.status.height = plan.height;
    s_map.status.planned_tiles = plan.count;
    set_message_locked("queued", "Loading current map view");
    xSemaphoreGive(s_map.lock);
    ret = d1l_map_prefetch_service_wake();
    if (ret != ESP_OK) {
        if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s_map.status.generation == generation) {
                s_map.status.visible = false;
                set_message_locked("worker_unavailable",
                                   "Map worker is unavailable");
            }
            xSemaphoreGive(s_map.lock);
        }
        return ret;
    }
    *out_generation = generation;
    return ESP_OK;
}

void d1l_map_view_service_force_reload_next_acquire(void)
{
    if (!s_map.lock) {
        return;
    }
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_map.force_reload_next_acquire = true;
        xSemaphoreGive(s_map.lock);
    }
}

void d1l_map_view_service_release_visible(uint32_t generation)
{
    if (!s_map.lock) {
        return;
    }
    /* Revoking the visible-map lease is the network safety boundary.  A
     * scheduling delay must not leave visible=true while a pending worker
     * notification is delivered.  Map lock sections are bounded and no
     * caller holds a frame while holding this mutex, so this cannot cycle. */
    if (xSemaphoreTake(s_map.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (generation == 0U || s_map.status.generation == generation) {
        s_map.status.visible = false;
        set_message_locked("hidden", "Map is not visible");
    }
    xSemaphoreGive(s_map.lock);
    (void)d1l_map_prefetch_service_wake();
}

bool d1l_map_view_service_visible(void)
{
    /*
     * Initialization publishes this never-deleted mutex only after every
     * allocation and status field is ready.
     */
    if (!s_map.lock) {
        return false;
    }
    if (xSemaphoreTake(s_map.lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool visible = s_map.status.visible;
    xSemaphoreGive(s_map.lock);
    return visible;
}

void d1l_map_view_service_status(d1l_map_view_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    if (!s_map.lock) {
        out_status->current_view_only = true;
        return;
    }
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out_status = s_map.status;
        xSemaphoreGive(s_map.lock);
    }
}

esp_err_t d1l_map_view_service_acquire_frame(uint32_t after_revision,
                                             d1l_map_view_frame_t *out_frame)
{
    if (!out_frame || !s_map.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_frame, 0, sizeof(*out_frame));
    if (xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_map.status.frame_ready || s_map.status.frame_revision <= after_revision) {
        xSemaphoreGive(s_map.lock);
        return ESP_ERR_NOT_FOUND;
    }
    const uint8_t slot = s_map.front_slot;
    if (s_map.frame_readers[slot] == UINT8_MAX) {
        xSemaphoreGive(s_map.lock);
        return ESP_ERR_INVALID_STATE;
    }
    ++s_map.frame_readers[slot];
    out_frame->pixels = s_map.frames[slot];
    out_frame->pixel_count = (size_t)s_map.status.width * s_map.status.height;
    out_frame->width = s_map.status.width;
    out_frame->height = s_map.status.height;
    out_frame->generation = s_map.status.generation;
    out_frame->revision = s_map.status.frame_revision;
    out_frame->slot = slot;
    out_frame->held = true;
    xSemaphoreGive(s_map.lock);
    return ESP_OK;
}

void d1l_map_view_service_release_frame(d1l_map_view_frame_t *frame)
{
    if (!frame || !frame->held || frame->slot > 1U || !s_map.lock) {
        return;
    }
    /* A frame pin must never be abandoned because a timed mutex acquisition
     * happened to lose a scheduling race.  Leaking the reader count can leave
     * the sole worker waiting forever when this slot becomes its back buffer.
     * The mutex is never held while a caller owns a frame, so waiting here
     * cannot form a lock cycle. */
    if (xSemaphoreTake(s_map.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_map.frame_readers[frame->slot] > 0U) {
        --s_map.frame_readers[frame->slot];
    }
    xSemaphoreGive(s_map.lock);
    memset(frame, 0, sizeof(*frame));
}
