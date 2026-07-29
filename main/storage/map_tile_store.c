#include "map_tile_store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "comms/connectivity_manager.h"
#include "map/map_tile_cache_policy.h"
#include "map/map_tile_provider.h"
#include "platform/time_service.h"
#include "storage/retained_blob_store.h"
#include "storage/storage_status_policy.h"

#if D1L_ENABLE_QUALIFICATION_HOOKS
#define D1L_MAP_TILE_CANARY_Z 12U
#define D1L_MAP_TILE_CANARY_X 1U
#define D1L_MAP_TILE_CANARY_Y 2U
#endif
#define D1L_MAP_TILE_HTTP_TIMEOUT_MS 15000
#define D1L_MAP_TILE_BACKGROUND_HTTP_TIMEOUT_MS 5000
#define D1L_MAP_TILE_HTTP_IO_SLICE_MS 250
#define D1L_MAP_TILE_REQUEST_GATE_SLICE_MS 25U
#define D1L_MAP_TILE_DEFAULT_RETRY_AFTER_SEC 300U
#define D1L_MAP_TILE_NETWORK_LEASE_TIMEOUT_MS 1000U
#define D1L_MAP_TILE_SD_FILE_TIMEOUT_MS 10000U
#define D1L_MAP_TILE_CACHE_JOURNAL_NAME "cache-journal.v1"
#define D1L_MAP_TILE_CACHE_JOURNAL_TMP_NAME "cache-journal-repair.tmp"
#define D1L_MAP_TILE_CACHE_STATE_NAME "cache-state.v1"
#define D1L_MAP_TILE_CACHE_STATE_TMP_NAME "cache-state.tmp"
#define D1L_MAP_TILE_STORE_TRANSACTION_TIMEOUT_MS 30000U

typedef struct {
    uint32_t token;
    int socket_fd;
    bool active;
    bool cancel_requested;
} d1l_map_background_socket_state_t;

static StaticSemaphore_t s_map_network_mutex_storage;
static SemaphoreHandle_t s_map_network_mutex;
static portMUX_TYPE s_map_network_init_lock =
    portMUX_INITIALIZER_UNLOCKED;
static d1l_map_background_socket_state_t s_background_socket = {
    .socket_fd = -1,
};
static uint32_t s_background_next_token;
static char s_request_gate_source[
    D1L_MAP_PROVIDER_SOURCE_ID_MAX + 1U];
static int64_t s_request_gate_minimum_until_us;
static int64_t s_request_gate_retry_until_us;
static int s_request_gate_retry_status;

static SemaphoreHandle_t map_network_mutex(void)
{
    if (!s_map_network_mutex) {
        portENTER_CRITICAL(&s_map_network_init_lock);
        if (!s_map_network_mutex) {
            s_map_network_mutex = xSemaphoreCreateMutexStatic(
                &s_map_network_mutex_storage);
        }
        portEXIT_CRITICAL(&s_map_network_init_lock);
    }
    return s_map_network_mutex;
}

static uint32_t background_fetch_begin(void)
{
    SemaphoreHandle_t mutex = map_network_mutex();
    if (!mutex ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return 0U;
    }
    uint32_t token = ++s_background_next_token;
    if (token == 0U) {
        token = ++s_background_next_token;
    }
    s_background_socket = (d1l_map_background_socket_state_t) {
        .token = token,
        .socket_fd = -1,
        .active = true,
        .cancel_requested = false,
    };
    xSemaphoreGive(mutex);
    return token;
}

static bool background_fetch_cancel_requested(uint32_t token)
{
    if (token == 0U) {
        return false;
    }
    SemaphoreHandle_t mutex = map_network_mutex();
    if (!mutex ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return true;
    }
    const bool cancelled =
        s_background_socket.active &&
        s_background_socket.token == token &&
        s_background_socket.cancel_requested;
    xSemaphoreGive(mutex);
    return cancelled;
}

static void background_fetch_publish_socket(
    uint32_t token,
    int socket_fd)
{
    if (token == 0U || socket_fd < 0) {
        return;
    }
    SemaphoreHandle_t mutex = map_network_mutex();
    if (!mutex ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_background_socket.active &&
        s_background_socket.token == token) {
        s_background_socket.socket_fd = socket_fd;
        if (s_background_socket.cancel_requested) {
            (void)shutdown(socket_fd, SHUT_RDWR);
        }
    }
    xSemaphoreGive(mutex);
}

static void background_fetch_clear_socket(
    uint32_t token,
    int socket_fd)
{
    if (token == 0U || socket_fd < 0) {
        return;
    }
    SemaphoreHandle_t mutex = map_network_mutex();
    if (!mutex ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_background_socket.active &&
        s_background_socket.token == token &&
        s_background_socket.socket_fd == socket_fd) {
        s_background_socket.socket_fd = -1;
    }
    xSemaphoreGive(mutex);
}

static void background_fetch_detach_socket(uint32_t token)
{
    if (token == 0U) {
        return;
    }
    SemaphoreHandle_t mutex = map_network_mutex();
    if (!mutex ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_background_socket.active &&
        s_background_socket.token == token) {
        s_background_socket.socket_fd = -1;
    }
    xSemaphoreGive(mutex);
}

static void background_fetch_finish(uint32_t token)
{
    if (token == 0U) {
        return;
    }
    SemaphoreHandle_t mutex = map_network_mutex();
    if (!mutex ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_background_socket.active &&
        s_background_socket.token == token) {
        s_background_socket =
            (d1l_map_background_socket_state_t) {
                .socket_fd = -1,
            };
    }
    xSemaphoreGive(mutex);
}

void d1l_map_tile_store_cancel_background_fetch(void)
{
    SemaphoreHandle_t mutex = map_network_mutex();
    if (!mutex ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_background_socket.active) {
        s_background_socket.cancel_requested = true;
        if (s_background_socket.socket_fd >= 0) {
            (void)shutdown(
                s_background_socket.socket_fd, SHUT_RDWR);
        }
    }
    xSemaphoreGive(mutex);
}

static esp_err_t request_gate_wait(
    const char *source_id,
    d1l_map_tile_continue_cb_t should_continue,
    void *continue_context,
    int *out_retry_status,
    uint32_t *out_retry_after_sec)
{
    if (!source_id || source_id[0] == '\0' ||
        !out_retry_status || !out_retry_after_sec) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_retry_status = 0;
    *out_retry_after_sec = 0U;
    for (;;) {
        if (should_continue &&
            !should_continue(continue_context)) {
            return ESP_ERR_INVALID_STATE;
        }
        SemaphoreHandle_t mutex = map_network_mutex();
        if (!mutex ||
            xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            return ESP_ERR_NO_MEM;
        }
        const bool source_matches =
            strcmp(s_request_gate_source, source_id) == 0;
        const int64_t minimum_until_us = source_matches ?
            s_request_gate_minimum_until_us : 0;
        const int64_t retry_until_us = source_matches ?
            s_request_gate_retry_until_us : 0;
        const int retry_status = source_matches ?
            s_request_gate_retry_status : 0;
        xSemaphoreGive(mutex);
        const int64_t now_us = esp_timer_get_time();
        const int64_t retry_remaining_us =
            retry_until_us - now_us;
        if (retry_remaining_us > 0 &&
            (retry_status == 429 || retry_status == 503)) {
            *out_retry_status = retry_status;
            *out_retry_after_sec = (uint32_t)(
                (retry_remaining_us + 999999LL) / 1000000LL);
            return ESP_ERR_TIMEOUT;
        }
        const int64_t remaining_us =
            minimum_until_us - now_us;
        if (remaining_us <= 0) {
            return ESP_OK;
        }
        uint32_t wait_ms =
            (uint32_t)((remaining_us + 999LL) / 1000LL);
        if (wait_ms > D1L_MAP_TILE_REQUEST_GATE_SLICE_MS) {
            wait_ms = D1L_MAP_TILE_REQUEST_GATE_SLICE_MS;
        }
        TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
        vTaskDelay(wait_ticks > 0U ? wait_ticks : 1U);
    }
}

static void request_gate_extend_minimum(
    const char *source_id,
    int64_t deadline_us)
{
    if (!source_id || source_id[0] == '\0') {
        return;
    }
    SemaphoreHandle_t mutex = map_network_mutex();
    if (!mutex ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (strcmp(s_request_gate_source, source_id) != 0) {
        snprintf(
            s_request_gate_source,
            sizeof(s_request_gate_source), "%s", source_id);
        s_request_gate_minimum_until_us = 0;
        s_request_gate_retry_until_us = 0;
        s_request_gate_retry_status = 0;
    }
    if (deadline_us > s_request_gate_minimum_until_us) {
        s_request_gate_minimum_until_us = deadline_us;
    }
    xSemaphoreGive(mutex);
}

static void request_gate_extend_retry(
    const char *source_id,
    int status_code,
    int64_t deadline_us)
{
    if (!source_id || source_id[0] == '\0' ||
        (status_code != 429 && status_code != 503)) {
        return;
    }
    SemaphoreHandle_t mutex = map_network_mutex();
    if (!mutex ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (strcmp(s_request_gate_source, source_id) != 0) {
        snprintf(
            s_request_gate_source,
            sizeof(s_request_gate_source), "%s", source_id);
        s_request_gate_minimum_until_us = 0;
        s_request_gate_retry_until_us = 0;
        s_request_gate_retry_status = 0;
    }
    if (deadline_us > s_request_gate_retry_until_us) {
        s_request_gate_retry_until_us = deadline_us;
        s_request_gate_retry_status = status_code;
    }
    xSemaphoreGive(mutex);
}

bool d1l_map_tile_store_coord_valid(uint8_t z, uint32_t x, uint32_t y)
{
    if (z > D1L_MAP_TILE_ZOOM_MAX) {
        return false;
    }
    const uint32_t limit = 1UL << z;
    return x < limit && y < limit;
}

bool d1l_map_tile_store_path(uint8_t z, uint32_t x, uint32_t y,
                             char *dest, size_t dest_size)
{
    if (!dest || dest_size == 0 || !d1l_map_tile_store_coord_valid(z, x, y)) {
        return false;
    }
    const int len = snprintf(dest, dest_size,
                             "map/tiles/openstreetmap/z%u/x%lu/y%lu.png",
                             (unsigned)z, (unsigned long)x, (unsigned long)y);
    return len > 0 && (size_t)len < dest_size;
}

static bool tile_result_paths(
    const d1l_map_tile_provider_t *provider,
    d1l_map_tile_download_result_t *result)
{
    if (!provider || !result ||
        !d1l_map_tile_store_coord_valid(result->z, result->x, result->y) ||
        !d1l_map_tile_provider_path(
            provider, result->z, result->x, result->y,
            result->path, sizeof(result->path))) {
        return false;
    }
    const size_t path_length = strlen(result->path);
    if (path_length < 4U ||
        strcmp(&result->path[path_length - 4U], ".png") != 0 ||
        path_length + 6U >= sizeof(result->metadata_tmp_path)) {
        return false;
    }
    const int tmp_written = snprintf(
        result->tmp_path, sizeof(result->tmp_path), "%.*s.tmp",
        (int)(path_length - 4U), result->path);
    const int metadata_written = snprintf(
        result->metadata_path, sizeof(result->metadata_path), "%.*s.meta",
        (int)(path_length - 4U), result->path);
    const int metadata_tmp_written = snprintf(
        result->metadata_tmp_path, sizeof(result->metadata_tmp_path),
        "%.*s.meta.tmp", (int)(path_length - 4U), result->path);
    if (tmp_written <= 0 || metadata_written <= 0 ||
        metadata_tmp_written <= 0 ||
        (size_t)tmp_written >= sizeof(result->tmp_path) ||
        (size_t)metadata_written >= sizeof(result->metadata_path) ||
        (size_t)metadata_tmp_written >= sizeof(result->metadata_tmp_path)) {
        return false;
    }
    return d1l_map_tile_provider_attribution_path(
               provider, false, result->attribution_path,
               sizeof(result->attribution_path)) &&
           d1l_map_tile_provider_attribution_path(
               provider, true, result->attribution_tmp_path,
               sizeof(result->attribution_tmp_path));
}

static bool append_text(char *dest, size_t dest_size, size_t *offset, const char *text)
{
    if (!dest || !offset || !text) {
        return false;
    }
    while (*text) {
        if (*offset + 1U >= dest_size) {
            return false;
        }
        dest[(*offset)++] = *text++;
    }
    dest[*offset] = '\0';
    return true;
}

static bool append_u32(char *dest, size_t dest_size, size_t *offset, uint32_t value)
{
    char token[12];
    const int len = snprintf(token, sizeof(token), "%lu", (unsigned long)value);
    return len > 0 && (size_t)len < sizeof(token) &&
           append_text(dest, dest_size, offset, token);
}

static bool build_tile_url(const char *url_template,
                           uint8_t z,
                           uint32_t x,
                           uint32_t y,
                           char *dest,
                           size_t dest_size)
{
    if (!url_template || !dest || dest_size == 0) {
        return false;
    }
    size_t out = 0;
    for (size_t i = 0; url_template[i] != '\0'; ++i) {
        if (strncmp(&url_template[i], "{z}", 3U) == 0) {
            if (!append_u32(dest, dest_size, &out, z)) {
                return false;
            }
            i += 2U;
        } else if (strncmp(&url_template[i], "{x}", 3U) == 0) {
            if (!append_u32(dest, dest_size, &out, x)) {
                return false;
            }
            i += 2U;
        } else if (strncmp(&url_template[i], "{y}", 3U) == 0) {
            if (!append_u32(dest, dest_size, &out, y)) {
                return false;
            }
            i += 2U;
        } else {
            char ch[2] = {url_template[i], '\0'};
            if (!append_text(dest, dest_size, &out, ch)) {
                return false;
            }
        }
    }
    return out > 0;
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static bool result_path_set(d1l_map_tile_canary_result_t *result,
                            const char *token)
{
    if (!result || !token || token[0] == '\0') {
        return false;
    }
    result->z = D1L_MAP_TILE_CANARY_Z;
    result->x = D1L_MAP_TILE_CANARY_X;
    result->y = D1L_MAP_TILE_CANARY_Y;
    const int final_len = snprintf(result->path, sizeof(result->path),
                                   "map/tiles/z%u/x%lu/y%lu-%s.tile",
                                   (unsigned)D1L_MAP_TILE_CANARY_Z,
                                   (unsigned long)D1L_MAP_TILE_CANARY_X,
                                   (unsigned long)D1L_MAP_TILE_CANARY_Y,
                                   token);
    const int tmp_len = snprintf(result->tmp_path, sizeof(result->tmp_path),
                                 "map/tiles/z%u/x%lu/y%lu-%s.tmp",
                                 (unsigned)D1L_MAP_TILE_CANARY_Z,
                                 (unsigned long)D1L_MAP_TILE_CANARY_X,
                                 (unsigned long)D1L_MAP_TILE_CANARY_Y,
                                 token);
    return final_len > 0 && tmp_len > 0 &&
           (size_t)final_len < sizeof(result->path) &&
           (size_t)tmp_len < sizeof(result->tmp_path);
}
#endif

static void download_step(d1l_map_tile_download_result_t *result,
                          const char *step,
                          esp_err_t ret,
                          const d1l_rp2040_file_result_t *file)
{
    if (!result) {
        return;
    }
    snprintf(result->step, sizeof(result->step), "%s", step ? step : "unknown");
    result->last_error = ret;
    if (file) {
        result->file = *file;
    }
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static void result_step(d1l_map_tile_canary_result_t *result,
                        const char *step,
                        esp_err_t ret,
                        const d1l_rp2040_file_result_t *file)
{
    if (!result) {
        return;
    }
    snprintf(result->step, sizeof(result->step), "%s", step ? step : "unknown");
    result->last_error = ret;
    if (file) {
        result->file = *file;
    }
}

static esp_err_t build_canary_payload(const char *token,
                                      uint8_t *payload,
                                      size_t payload_size,
                                      size_t *payload_len)
{
    if (!token || !payload || !payload_len) {
        return ESP_ERR_INVALID_ARG;
    }
    const int len = snprintf((char *)payload, payload_size,
                             "{\"schema\":1,\"kind\":\"map_tile_cache_canary\","
                             "\"token\":\"%s\",\"z\":%u,\"x\":%lu,\"y\":%lu,"
                             "\"public_rf_tx\":false,\"formats_sd\":false}\n",
                             token,
                             (unsigned)D1L_MAP_TILE_CANARY_Z,
                             (unsigned long)D1L_MAP_TILE_CANARY_X,
                             (unsigned long)D1L_MAP_TILE_CANARY_Y);
    if (len <= 0 || (size_t)len >= payload_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    *payload_len = (size_t)len;
    return ESP_OK;
}

bool d1l_map_tile_store_token_valid(const char *token)
{
    if (!token || token[0] == '\0') {
        return false;
    }
    size_t len = 0;
    while (token[len] != '\0') {
        const unsigned char ch = (unsigned char)token[len];
        if (!(isalnum(ch) || ch == '-' || ch == '_' || ch == '.')) {
            return false;
        }
        len++;
        if (len > D1L_MAP_TILE_CANARY_TOKEN_MAX) {
            return false;
        }
    }
    return true;
}
#endif

bool d1l_map_tile_store_sd_ready(const d1l_storage_status_t *status)
{
    return status &&
           d1l_storage_status_policy_allows_cached_io(
               status->bridge_status_refresh_failures) &&
           status->sd_present &&
           status->sd_mounted &&
           status->sd_data_root_ready &&
           status->rp2040_sd_protocol_supported &&
           status->file_ops_supported &&
           status->atomic_rename_supported &&
           status->file_line_max >= D1L_RP2040_FILE_LINE_MAX &&
           status->file_chunk_max >= D1L_RP2040_FILE_CHUNK_MAX &&
           status->path_max >= D1L_RP2040_FILE_PATH_MAX;
}

static esp_err_t write_attribution_metadata(
    const d1l_map_tile_provider_t *provider,
    d1l_map_tile_download_result_t *result,
    d1l_map_tile_continue_cb_t should_continue,
    void *continue_context)
{
    if (!provider || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    char payload[512];
    const int len = snprintf(payload, sizeof(payload),
                             "{\"schema\":1,\"kind\":\"map_tile_attribution\","
                             "\"source\":\"%s\",\"attribution\":\"%s\","
                             "\"license_url\":\"%s\",\"policy\":\"%s\","
                             "\"min_cache_days\":%u,\"current_view_only\":%s,"
                             "\"background_prefetch_permitted\":%s,"
                             "\"public_rf_tx\":false,\"formats_sd\":false}\n",
                             provider->source_id,
                             provider->attribution,
                             provider->license_url,
                             provider->configured ?
                                 "configured_offline_provider" :
                                 D1L_MAP_TILE_PROVIDER_POLICY,
                             (unsigned)D1L_MAP_TILE_MIN_CACHE_DAYS,
                             provider->configured ? "false" : "true",
                             provider->background_prefetch_permitted ?
                                 "true" : "false");
    if (len <= 0 || (size_t)len >= sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (should_continue &&
        !should_continue(continue_context)) {
        result->cancelled = true;
        return ESP_ERR_INVALID_STATE;
    }
    d1l_rp2040_file_result_t file = {0};
    (void)d1l_rp2040_bridge_file_delete(result->attribution_tmp_path, &file,
                                        D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    size_t offset = 0U;
    while (offset < (size_t)len) {
        if (should_continue &&
            !should_continue(continue_context)) {
            result->cancelled = true;
            return ESP_ERR_INVALID_STATE;
        }
        const size_t remaining = (size_t)len - offset;
        const size_t chunk = remaining < D1L_RP2040_FILE_CHUNK_MAX ?
                             remaining : D1L_RP2040_FILE_CHUNK_MAX;
        esp_err_t ret = d1l_rp2040_bridge_file_write(
            result->attribution_tmp_path, (uint32_t)offset,
            (const uint8_t *)&payload[offset], chunk, offset == 0U, &file,
            D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        if (ret != ESP_OK || file.length != (uint32_t)chunk) {
            download_step(result, "write_attribution", ret == ESP_OK ? ESP_FAIL : ret, &file);
            (void)d1l_rp2040_bridge_file_delete(result->attribution_tmp_path, &file,
                                                D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
            return result->last_error;
        }
        offset += chunk;
    }
    if (should_continue &&
        !should_continue(continue_context)) {
        result->cancelled = true;
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = d1l_rp2040_bridge_file_rename(result->attribution_tmp_path,
                                                  result->attribution_path, true,
                                                  &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        download_step(result, "rename_attribution", ret, &file);
        (void)d1l_rp2040_bridge_file_delete(result->attribution_tmp_path, &file,
                                            D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        return ret;
    }
    result->attribution_saved = true;
    return ESP_OK;
}

bool d1l_map_tile_png_valid(const uint8_t *data, size_t len)
{
    static const uint8_t signature[] = {0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    if (!data || len < 24U || memcmp(data, signature, sizeof(signature)) != 0 ||
        memcmp(&data[12], "IHDR", 4U) != 0) {
        return false;
    }
    const uint32_t width = ((uint32_t)data[16] << 24U) |
                           ((uint32_t)data[17] << 16U) |
                           ((uint32_t)data[18] << 8U) |
                           (uint32_t)data[19];
    const uint32_t height = ((uint32_t)data[20] << 24U) |
                            ((uint32_t)data[21] << 16U) |
                            ((uint32_t)data[22] << 8U) |
                            (uint32_t)data[23];
    return width == 256U && height == 256U;
}

static bool continue_allowed(d1l_map_tile_continue_cb_t should_continue, void *context)
{
    return !should_continue || should_continue(context);
}

typedef struct {
    d1l_map_tile_continue_cb_t caller;
    void *caller_context;
    uint32_t background_token;
} d1l_map_network_continue_t;

static bool map_network_continue(void *context)
{
    const d1l_map_network_continue_t *continuation =
        (const d1l_map_network_continue_t *)context;
    return continuation &&
           !background_fetch_cancel_requested(
               continuation->background_token) &&
           !d1l_connectivity_network_cancel_requested() &&
           continue_allowed(
               continuation->caller, continuation->caller_context);
}

static void init_download_result(d1l_map_tile_download_result_t *result,
                                 const d1l_map_tile_provider_t *provider,
                                 uint8_t z,
                                 uint32_t x,
                                 uint32_t y,
                                 const d1l_storage_status_t *status,
                                 bool wifi_connected)
{
    if (!result || !provider) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->z = z;
    result->x = x;
    result->y = y;
    result->wifi_connected = wifi_connected;
    result->sd_ready = d1l_map_tile_store_sd_ready(status);
    result->provider_allowed = provider->network_fetch_allowed;
    result->provider_configured = provider->configured;
    result->background_prefetch_permitted =
        provider->background_prefetch_permitted;
    result->cache_budget_bytes =
        (uint64_t)provider->cache_budget_mb * 1024ULL * 1024ULL;
    result->public_rf_tx = false;
    result->formats_sd = false;
    result->last_error = ESP_OK;
    snprintf(result->source_id, sizeof(result->source_id), "%s",
             provider->source_id);
    snprintf(result->attribution, sizeof(result->attribution), "%s",
             provider->attribution);
    snprintf(result->license_url, sizeof(result->license_url), "%s",
             provider->license_url);
    (void)tile_result_paths(provider, result);
}

static void cleanup_partial(const d1l_map_tile_download_result_t *result)
{
    if (!result || !result->tmp_path[0]) {
        return;
    }
    d1l_rp2040_file_result_t file = {0};
    (void)d1l_rp2040_bridge_file_delete(result->tmp_path, &file,
                                        D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (result->metadata_tmp_path[0]) {
        (void)d1l_rp2040_bridge_file_delete(
            result->metadata_tmp_path, &file,
            D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    }
}

typedef struct {
    char journal[D1L_RP2040_FILE_PATH_MAX + 1U];
    char journal_tmp[D1L_RP2040_FILE_PATH_MAX + 1U];
    char state[D1L_RP2040_FILE_PATH_MAX + 1U];
    char state_tmp[D1L_RP2040_FILE_PATH_MAX + 1U];
} d1l_map_tile_cache_paths_t;

static StaticSemaphore_t s_cache_transaction_mutex_storage;
static SemaphoreHandle_t s_cache_transaction_mutex;
static portMUX_TYPE s_cache_transaction_init_lock =
    portMUX_INITIALIZER_UNLOCKED;
static d1l_map_tile_cache_state_t s_cache_state;
static bool s_cache_state_loaded;
static char s_cache_state_source[D1L_MAP_PROVIDER_SOURCE_ID_MAX + 1U];
static uint32_t s_cache_state_capacity_kb;
static uint32_t s_cache_state_backend_generation;

static esp_err_t cache_backend_generation(uint32_t *generation)
{
    if (!generation) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_retained_blob_store_backend_state_t backend = {0};
    if (!d1l_retained_blob_store_backend_state(
            D1L_RETAINED_BLOB_STORE_ROUTES, &backend) ||
        !backend.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    *generation = backend.generation;
    return ESP_OK;
}

static bool cache_backend_generation_matches(uint32_t expected)
{
    uint32_t current = 0U;
    return cache_backend_generation(&current) == ESP_OK &&
           current == expected;
}

static esp_err_t cache_transaction_take(void)
{
    if (!s_cache_transaction_mutex) {
        portENTER_CRITICAL(&s_cache_transaction_init_lock);
        if (!s_cache_transaction_mutex) {
            s_cache_transaction_mutex = xSemaphoreCreateMutexStatic(
                &s_cache_transaction_mutex_storage);
        }
        portEXIT_CRITICAL(&s_cache_transaction_init_lock);
    }
    if (!s_cache_transaction_mutex) {
        return ESP_ERR_NO_MEM;
    }
    TickType_t ticks = pdMS_TO_TICKS(
        D1L_MAP_TILE_STORE_TRANSACTION_TIMEOUT_MS);
    if (ticks == 0U) {
        ticks = 1U;
    }
    return xSemaphoreTake(s_cache_transaction_mutex, ticks) == pdTRUE ?
        ESP_OK : ESP_ERR_TIMEOUT;
}

static void cache_transaction_give(void)
{
    if (s_cache_transaction_mutex) {
        xSemaphoreGive(s_cache_transaction_mutex);
    }
}

static bool persistence_continue(
    d1l_map_tile_download_result_t *result,
    d1l_map_tile_continue_cb_t should_continue,
    void *continue_context)
{
    if (!result || result->cache_intent_recorded) {
        return true;
    }
    if (continue_allowed(
            should_continue, continue_context)) {
        return true;
    }
    result->cancelled = true;
    return false;
}

static esp_err_t cache_transaction_take_cancelable(
    d1l_map_tile_download_result_t *result,
    d1l_map_tile_continue_cb_t should_continue,
    void *continue_context)
{
    if (!persistence_continue(
            result, should_continue, continue_context)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_cache_transaction_mutex) {
        portENTER_CRITICAL(&s_cache_transaction_init_lock);
        if (!s_cache_transaction_mutex) {
            s_cache_transaction_mutex =
                xSemaphoreCreateMutexStatic(
                    &s_cache_transaction_mutex_storage);
        }
        portEXIT_CRITICAL(&s_cache_transaction_init_lock);
    }
    if (!s_cache_transaction_mutex) {
        return ESP_ERR_NO_MEM;
    }
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)D1L_MAP_TILE_STORE_TRANSACTION_TIMEOUT_MS *
            1000LL;
    for (;;) {
        if (!persistence_continue(
                result, should_continue, continue_context)) {
            return ESP_ERR_INVALID_STATE;
        }
        const int64_t remaining_us =
            deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        uint32_t wait_ms =
            (uint32_t)((remaining_us + 999LL) / 1000LL);
        if (wait_ms > D1L_MAP_TILE_REQUEST_GATE_SLICE_MS) {
            wait_ms = D1L_MAP_TILE_REQUEST_GATE_SLICE_MS;
        }
        TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
        if (xSemaphoreTake(
                s_cache_transaction_mutex,
                wait_ticks > 0U ? wait_ticks : 1U) == pdTRUE) {
            return ESP_OK;
        }
    }
}

static bool cache_control_paths(
    const d1l_map_tile_provider_t *provider,
    d1l_map_tile_cache_paths_t *paths)
{
    if (!provider || !paths || provider->source_id[0] == '\0') {
        return false;
    }
    const char *directory = provider->configured ?
        provider->source_id : "openstreetmap";
    const int journal = snprintf(
        paths->journal, sizeof(paths->journal),
        "map/tiles/%s/%s", directory,
        D1L_MAP_TILE_CACHE_JOURNAL_NAME);
    const int journal_tmp = snprintf(
        paths->journal_tmp, sizeof(paths->journal_tmp),
        "map/tiles/%s/%s", directory,
        D1L_MAP_TILE_CACHE_JOURNAL_TMP_NAME);
    const int state = snprintf(
        paths->state, sizeof(paths->state),
        "map/tiles/%s/%s", directory,
        D1L_MAP_TILE_CACHE_STATE_NAME);
    const int state_tmp = snprintf(
        paths->state_tmp, sizeof(paths->state_tmp),
        "map/tiles/%s/%s", directory,
        D1L_MAP_TILE_CACHE_STATE_TMP_NAME);
    return journal > 0 && journal_tmp > 0 &&
           state > 0 && state_tmp > 0 &&
           (size_t)journal < sizeof(paths->journal) &&
           (size_t)journal_tmp < sizeof(paths->journal_tmp) &&
           (size_t)state < sizeof(paths->state) &&
           (size_t)state_tmp < sizeof(paths->state_tmp);
}

static bool cache_records_equal(
    const d1l_map_tile_cache_record_t *left,
    const d1l_map_tile_cache_record_t *right)
{
    return left && right &&
           left->sequence == right->sequence &&
           left->size == right->size &&
           left->content_crc32 == right->content_crc32 &&
           left->zoom == right->zoom &&
           left->x == right->x &&
           left->y == right->y;
}

static bool cache_record_matches_tile(
    const d1l_map_tile_cache_record_t *record,
    uint8_t z,
    uint32_t x,
    uint32_t y)
{
    return record && record->zoom == z &&
           record->x == x && record->y == y;
}

static bool file_result_missing(
    esp_err_t ret,
    const d1l_rp2040_file_result_t *file)
{
    return ret == ESP_ERR_NOT_FOUND ||
           (ret == ESP_OK && file && file->ok &&
            !file->exists) ||
           (file && strcmp(file->err, "not_found") == 0);
}

static esp_err_t delete_file_allow_missing(const char *path)
{
    if (!path || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_rp2040_file_result_t file = {0};
    const esp_err_t ret = d1l_rp2040_bridge_file_delete(
        path, &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    return file_result_missing(ret, &file) ? ESP_OK : ret;
}

static esp_err_t read_file_exact(
    const char *path,
    uint32_t offset,
    uint8_t *buffer,
    size_t length)
{
    if (!path || !buffer || length == 0U ||
        offset > UINT32_MAX - length) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t done = 0U;
    while (done < length) {
        const size_t remaining = length - done;
        const size_t chunk = remaining < D1L_RP2040_FILE_CHUNK_MAX ?
            remaining : D1L_RP2040_FILE_CHUNK_MAX;
        d1l_rp2040_file_result_t file = {0};
        const esp_err_t ret = d1l_rp2040_bridge_file_read(
            path, offset + (uint32_t)done, &buffer[done],
            chunk, &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        if (ret != ESP_OK || !file.ok ||
            file.offset != offset + (uint32_t)done ||
            file.length == 0U || file.length > chunk) {
            return ret == ESP_OK ? ESP_FAIL : ret;
        }
        done += file.length;
    }
    return ESP_OK;
}

static esp_err_t read_cache_record(
    const char *path,
    uint32_t offset,
    d1l_map_tile_cache_record_t *record)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t encoded[D1L_MAP_TILE_CACHE_RECORD_BYTES];
    const esp_err_t ret = read_file_exact(
        path, offset, encoded, sizeof(encoded));
    if (ret != ESP_OK) {
        return ret;
    }
    return d1l_map_tile_cache_record_decode(encoded, record) ?
        ESP_OK : ESP_ERR_INVALID_CRC;
}

static esp_err_t read_cache_metadata(
    const char *path,
    d1l_map_tile_cache_record_t *record)
{
    if (!path || !record) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_rp2040_file_result_t file = {0};
    const esp_err_t stat_ret = d1l_rp2040_bridge_file_stat(
        path, &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (file_result_missing(stat_ret, &file)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (stat_ret != ESP_OK || !file.ok || !file.exists ||
        file.is_directory ||
        file.size != D1L_MAP_TILE_CACHE_RECORD_BYTES) {
        return stat_ret == ESP_OK ? ESP_ERR_INVALID_SIZE : stat_ret;
    }
    return read_cache_record(path, 0U, record);
}

static esp_err_t write_atomic_blob(
    const char *path,
    const char *tmp_path,
    const uint8_t *data,
    size_t length)
{
    if (!path || !tmp_path || !data || length == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = delete_file_allow_missing(tmp_path);
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_rp2040_file_result_t file = {0};
    size_t offset = 0U;
    while (offset < length) {
        const size_t remaining = length - offset;
        const size_t chunk = remaining < D1L_RP2040_FILE_CHUNK_MAX ?
            remaining : D1L_RP2040_FILE_CHUNK_MAX;
        ret = d1l_rp2040_bridge_file_write(
            tmp_path, (uint32_t)offset, &data[offset], chunk,
            offset == 0U, &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        if (ret != ESP_OK || file.length != (uint32_t)chunk) {
            (void)delete_file_allow_missing(tmp_path);
            return ret == ESP_OK ? ESP_FAIL : ret;
        }
        offset += chunk;
    }
    uint8_t verify[D1L_MAP_TILE_CACHE_STATE_BYTES];
    if (length > sizeof(verify)) {
        (void)delete_file_allow_missing(tmp_path);
        return ESP_ERR_INVALID_SIZE;
    }
    ret = read_file_exact(tmp_path, 0U, verify, length);
    if (ret != ESP_OK || memcmp(data, verify, length) != 0) {
        memset(verify, 0, sizeof(verify));
        (void)delete_file_allow_missing(tmp_path);
        return ret == ESP_OK ? ESP_ERR_INVALID_CRC : ret;
    }
    memset(verify, 0, sizeof(verify));
    ret = d1l_rp2040_bridge_file_rename(
        tmp_path, path, true, &file,
        D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        (void)delete_file_allow_missing(tmp_path);
    }
    return ret;
}

static esp_err_t write_cache_state(
    const d1l_map_tile_cache_paths_t *paths,
    const d1l_map_tile_cache_state_t *state)
{
    if (!paths || !state) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t encoded[D1L_MAP_TILE_CACHE_STATE_BYTES];
    if (!d1l_map_tile_cache_state_encode(state, encoded)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = write_atomic_blob(
        paths->state, paths->state_tmp, encoded, sizeof(encoded));
    memset(encoded, 0, sizeof(encoded));
    return ret;
}

static esp_err_t write_cache_metadata_tmp(
    const d1l_map_tile_download_result_t *result,
    const d1l_map_tile_cache_record_t *record)
{
    if (!result || !record ||
        result->metadata_tmp_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t encoded[D1L_MAP_TILE_CACHE_RECORD_BYTES];
    if (!d1l_map_tile_cache_record_encode(record, encoded)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = delete_file_allow_missing(
        result->metadata_tmp_path);
    if (ret != ESP_OK) {
        memset(encoded, 0, sizeof(encoded));
        return ret;
    }
    d1l_rp2040_file_result_t file = {0};
    ret = d1l_rp2040_bridge_file_write(
        result->metadata_tmp_path, 0U, encoded, sizeof(encoded),
        true, &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret == ESP_OK &&
        (file.length != sizeof(encoded) ||
         file.size != sizeof(encoded))) {
        ret = ESP_FAIL;
    }
    d1l_map_tile_cache_record_t verify = {0};
    if (ret == ESP_OK) {
        ret = read_cache_metadata(
            result->metadata_tmp_path, &verify);
    }
    if (ret == ESP_OK &&
        !cache_records_equal(record, &verify)) {
        ret = ESP_ERR_INVALID_CRC;
    }
    memset(encoded, 0, sizeof(encoded));
    memset(&verify, 0, sizeof(verify));
    if (ret != ESP_OK) {
        (void)delete_file_allow_missing(
            result->metadata_tmp_path);
    }
    return ret;
}

static esp_err_t append_cache_intent(
    const d1l_map_tile_cache_paths_t *paths,
    const d1l_map_tile_cache_state_t *state,
    const d1l_map_tile_cache_record_t *record)
{
    if (!paths || !state || !record) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t encoded[D1L_MAP_TILE_CACHE_RECORD_BYTES];
    if (!d1l_map_tile_cache_record_encode(record, encoded)) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_rp2040_file_result_t file = {0};
    const esp_err_t ret = d1l_rp2040_bridge_file_append(
        paths->journal, encoded, sizeof(encoded), &file,
        D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    memset(encoded, 0, sizeof(encoded));
    if (ret != ESP_OK) {
        return ret;
    }
    return file.offset == state->tail_offset &&
           file.length == D1L_MAP_TILE_CACHE_RECORD_BYTES &&
           file.size ==
               state->tail_offset + D1L_MAP_TILE_CACHE_RECORD_BYTES ?
        ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t rename_cache_metadata(
    const d1l_map_tile_download_result_t *result)
{
    if (!result || result->metadata_path[0] == '\0' ||
        result->metadata_tmp_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_rp2040_file_result_t file = {0};
    return d1l_rp2040_bridge_file_rename(
        result->metadata_tmp_path, result->metadata_path, true,
        &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
}

static esp_err_t verify_tile_file_continue(
    const char *path,
    const d1l_map_tile_cache_record_t *record,
    d1l_map_tile_download_result_t *result,
    d1l_map_tile_continue_cb_t should_continue,
    void *continue_context)
{
    if (!path || !record) {
        return ESP_ERR_INVALID_ARG;
    }
    if (result &&
        !persistence_continue(
            result, should_continue, continue_context)) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_rp2040_file_result_t file = {0};
    esp_err_t ret = d1l_rp2040_bridge_file_stat(
        path, &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (file_result_missing(ret, &file)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (ret != ESP_OK || !file.ok || !file.exists ||
        file.is_directory || file.size != record->size ||
        file.size > D1L_MAP_TILE_DOWNLOAD_MAX_BYTES) {
        return ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret;
    }
    uint8_t chunk[D1L_RP2040_FILE_CHUNK_MAX];
    uint8_t header[24] = {0};
    size_t header_length = 0U;
    uint32_t crc = 0U;
    uint32_t offset = 0U;
    while (offset < record->size) {
        if (result &&
            !persistence_continue(
                result, should_continue, continue_context)) {
            memset(chunk, 0, sizeof(chunk));
            return ESP_ERR_INVALID_STATE;
        }
        const size_t remaining = (size_t)record->size - offset;
        const size_t wanted = remaining < sizeof(chunk) ?
            remaining : sizeof(chunk);
        d1l_rp2040_file_result_t read = {0};
        ret = d1l_rp2040_bridge_file_read(
            path, offset, chunk, wanted, &read,
            D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        if (ret != ESP_OK || !read.ok ||
            read.offset != offset || read.length == 0U ||
            read.length > wanted) {
            memset(chunk, 0, sizeof(chunk));
            return ret == ESP_OK ? ESP_FAIL : ret;
        }
        if (header_length < sizeof(header)) {
            const size_t header_remaining =
                sizeof(header) - header_length;
            const size_t copy = read.length < header_remaining ?
                read.length : header_remaining;
            memcpy(&header[header_length], chunk, copy);
            header_length += copy;
        }
        crc = d1l_map_tile_cache_crc32_update(
            crc, chunk, read.length);
        offset += read.length;
    }
    memset(chunk, 0, sizeof(chunk));
    return header_length == sizeof(header) &&
           d1l_map_tile_png_valid(header, sizeof(header)) &&
           crc == record->content_crc32 ?
        ESP_OK : ESP_ERR_INVALID_CRC;
}

static esp_err_t verify_tile_file(
    const char *path,
    const d1l_map_tile_cache_record_t *record)
{
    return verify_tile_file_continue(
        path, record, NULL, NULL, NULL);
}

static esp_err_t recover_interrupted_record(
    const d1l_map_tile_provider_t *provider,
    const d1l_map_tile_cache_record_t *record)
{
    if (!provider || !record) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_map_tile_download_result_t result = {
        .z = record->zoom,
        .x = record->x,
        .y = record->y,
    };
    if (!tile_result_paths(provider, &result)) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_map_tile_cache_record_t final_metadata = {0};
    d1l_map_tile_cache_record_t temporary_metadata = {0};
    const esp_err_t final_metadata_ret = read_cache_metadata(
        result.metadata_path, &final_metadata);
    const esp_err_t temporary_metadata_ret = read_cache_metadata(
        result.metadata_tmp_path, &temporary_metadata);
    const bool final_metadata_matches =
        final_metadata_ret == ESP_OK &&
        cache_records_equal(record, &final_metadata);
    const bool temporary_metadata_matches =
        temporary_metadata_ret == ESP_OK &&
        cache_records_equal(record, &temporary_metadata);
    const esp_err_t final_tile_ret =
        verify_tile_file(result.path, record);
    const esp_err_t temporary_tile_ret =
        verify_tile_file(result.tmp_path, record);
    d1l_map_tile_cache_recovery_plan_t recovery = {0};
    if (!d1l_map_tile_cache_recovery_plan(
            final_tile_ret == ESP_OK,
            temporary_tile_ret == ESP_OK,
            final_metadata_matches,
            temporary_metadata_matches,
            &recovery)) {
        if (!final_metadata_matches &&
            !temporary_metadata_matches) {
            const esp_err_t reason =
                temporary_metadata_ret != ESP_ERR_NOT_FOUND ?
                    temporary_metadata_ret : final_metadata_ret;
            return reason == ESP_OK ?
                ESP_ERR_INVALID_CRC : reason;
        }
        return temporary_tile_ret != ESP_ERR_NOT_FOUND ?
            temporary_tile_ret : final_tile_ret;
    }
    esp_err_t ret = ESP_OK;
    if (recovery.rename_tile) {
        d1l_rp2040_file_result_t file = {0};
        ret = d1l_rp2040_bridge_file_rename(
            result.tmp_path, result.path, true, &file,
            D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (recovery.rename_metadata) {
        ret = rename_cache_metadata(&result);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    (void)delete_file_allow_missing(result.tmp_path);
    (void)delete_file_allow_missing(result.metadata_tmp_path);
    return ESP_OK;
}

static esp_err_t rebuild_cache_journal_prefix(
    const d1l_map_tile_cache_paths_t *paths,
    uint32_t valid_prefix_bytes)
{
    if (!paths ||
        valid_prefix_bytes % D1L_MAP_TILE_CACHE_RECORD_BYTES != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = delete_file_allow_missing(paths->journal_tmp);
    if (ret != ESP_OK) {
        return ret;
    }
    if (valid_prefix_bytes == 0U) {
        return delete_file_allow_missing(paths->journal);
    }
    uint8_t encoded[D1L_RP2040_FILE_CHUNK_MAX];
    uint8_t verify[D1L_RP2040_FILE_CHUNK_MAX];
    d1l_rp2040_file_result_t file = {0};
    for (uint32_t offset = 0U; offset < valid_prefix_bytes;) {
        const uint32_t remaining = valid_prefix_bytes - offset;
        const size_t chunk =
            remaining < sizeof(encoded) ? remaining : sizeof(encoded);
        ret = read_file_exact(
            paths->journal, offset, encoded, chunk);
        if (ret != ESP_OK) {
            goto rebuild_failed;
        }
        ret = d1l_rp2040_bridge_file_write(
            paths->journal_tmp, offset, encoded, chunk,
            offset == 0U, &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        if (ret != ESP_OK ||
            file.offset != offset ||
            file.length != chunk ||
            file.size != offset + chunk) {
            ret = ret == ESP_OK ? ESP_FAIL : ret;
            goto rebuild_failed;
        }
        ret = read_file_exact(
            paths->journal_tmp, offset, verify, chunk);
        if (ret != ESP_OK ||
            memcmp(encoded, verify, chunk) != 0) {
            ret = ret == ESP_OK ? ESP_ERR_INVALID_CRC : ret;
            goto rebuild_failed;
        }
        offset += (uint32_t)chunk;
    }
    memset(encoded, 0, sizeof(encoded));
    memset(verify, 0, sizeof(verify));
    ret = d1l_rp2040_bridge_file_rename(
        paths->journal_tmp, paths->journal, true, &file,
        D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        (void)delete_file_allow_missing(paths->journal_tmp);
    }
    return ret;

rebuild_failed:
    memset(encoded, 0, sizeof(encoded));
    memset(verify, 0, sizeof(verify));
    (void)delete_file_allow_missing(paths->journal_tmp);
    return ret;
}

static uint32_t cache_next_sequence(uint32_t sequence)
{
    return sequence == UINT32_MAX ? 1U : sequence + 1U;
}

static esp_err_t repair_cache_journal(
    const d1l_map_tile_cache_paths_t *paths,
    const d1l_map_tile_cache_state_t *state,
    uint32_t *size)
{
    if (!paths || !state || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    *size = 0U;
    d1l_rp2040_file_result_t file = {0};
    const esp_err_t ret = d1l_rp2040_bridge_file_stat(
        paths->journal, &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (file_result_missing(ret, &file)) {
        (void)delete_file_allow_missing(paths->journal_tmp);
        return ESP_OK;
    }
    if (ret != ESP_OK || !file.ok || !file.exists ||
        file.is_directory) {
        return ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret;
    }

    const uint32_t complete_bytes =
        file.size -
        (file.size % D1L_MAP_TILE_CACHE_RECORD_BYTES);
    if (state->tail_offset > complete_bytes) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t valid_prefix_bytes = state->tail_offset;
    uint32_t expected_sequence = state->next_sequence;
    while (valid_prefix_bytes < complete_bytes) {
        d1l_map_tile_cache_record_t record = {0};
        const esp_err_t read_ret = read_cache_record(
            paths->journal, valid_prefix_bytes, &record);
        if (read_ret == ESP_ERR_INVALID_CRC ||
            read_ret == ESP_ERR_INVALID_SIZE) {
            break;
        }
        if (read_ret != ESP_OK) {
            return read_ret;
        }
        if (record.sequence != expected_sequence) {
            break;
        }
        expected_sequence = cache_next_sequence(record.sequence);
        valid_prefix_bytes += D1L_MAP_TILE_CACHE_RECORD_BYTES;
    }

    d1l_map_tile_cache_journal_repair_plan_t plan = {0};
    if (!d1l_map_tile_cache_journal_repair_plan(
            file.size, state->tail_offset,
            valid_prefix_bytes, &plan)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (plan.rebuild) {
        const esp_err_t rebuild_ret = rebuild_cache_journal_prefix(
            paths, plan.valid_prefix_bytes);
        if (rebuild_ret != ESP_OK) {
            return rebuild_ret;
        }
    } else {
        (void)delete_file_allow_missing(paths->journal_tmp);
    }
    *size = plan.valid_prefix_bytes;
    return ESP_OK;
}

static esp_err_t repair_cache_head(
    const d1l_map_tile_provider_t *provider,
    const d1l_map_tile_cache_paths_t *paths,
    d1l_map_tile_cache_state_t *state)
{
    if (!provider || !paths || !state) {
        return ESP_ERR_INVALID_ARG;
    }
    bool changed = false;
    while (state->head_offset < state->tail_offset) {
        d1l_map_tile_cache_record_t record = {0};
        esp_err_t ret = read_cache_record(
            paths->journal, state->head_offset, &record);
        if (ret != ESP_OK) {
            return ret;
        }
        d1l_map_tile_download_result_t result = {
            .z = record.zoom,
            .x = record.x,
            .y = record.y,
        };
        if (!tile_result_paths(provider, &result)) {
            return ESP_ERR_INVALID_ARG;
        }
        d1l_map_tile_cache_record_t metadata = {0};
        const esp_err_t metadata_ret = read_cache_metadata(
            result.metadata_path, &metadata);
        d1l_rp2040_file_result_t tile = {0};
        const esp_err_t tile_ret = d1l_rp2040_bridge_file_stat(
            result.path, &tile, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        const bool tile_missing =
            file_result_missing(tile_ret, &tile);
        if (tile_ret != ESP_OK && !tile_missing) {
            return tile_ret;
        }
        if (metadata_ret == ESP_OK &&
            cache_records_equal(&record, &metadata) &&
            tile_ret == ESP_OK && tile.ok && tile.exists &&
            !tile.is_directory && tile.size == record.size) {
            break;
        }
        if (metadata_ret == ESP_OK &&
            cache_records_equal(&record, &metadata) &&
            tile_missing) {
            ret = delete_file_allow_missing(result.metadata_path);
            if (ret != ESP_OK) {
                return ret;
            }
        } else if (metadata_ret == ESP_OK &&
                   cache_records_equal(&record, &metadata) &&
                   !tile_missing) {
            ret = delete_file_allow_missing(result.path);
            if (ret == ESP_OK) {
                ret = delete_file_allow_missing(
                    result.metadata_path);
            }
            if (ret != ESP_OK) {
                return ret;
            }
        } else if (metadata_ret == ESP_ERR_NOT_FOUND &&
                   !tile_missing) {
            return ESP_ERR_INVALID_STATE;
        } else if (metadata_ret != ESP_OK &&
                   metadata_ret != ESP_ERR_NOT_FOUND) {
            return metadata_ret;
        }
        if (!d1l_map_tile_cache_state_note_evict(
                state, &record)) {
            return ESP_ERR_INVALID_STATE;
        }
        changed = true;
    }
    return changed ? write_cache_state(paths, state) : ESP_OK;
}

static esp_err_t recover_cache_tail(
    const d1l_map_tile_provider_t *provider,
    const d1l_map_tile_cache_paths_t *paths,
    uint32_t journal_size,
    d1l_map_tile_cache_state_t *state)
{
    if (!provider || !paths || !state ||
        state->tail_offset > journal_size) {
        return ESP_ERR_INVALID_ARG;
    }
    while (state->tail_offset < journal_size) {
        d1l_map_tile_cache_record_t record = {0};
        esp_err_t ret = read_cache_record(
            paths->journal, state->tail_offset, &record);
        if (ret != ESP_OK) {
            return ret;
        }
        if (record.sequence != state->next_sequence) {
            return ESP_ERR_INVALID_STATE;
        }
        ret = recover_interrupted_record(provider, &record);
        if (ret != ESP_OK) {
            return ret;
        }
        if (!d1l_map_tile_cache_state_note_commit(
                state, &record)) {
            return ESP_ERR_INVALID_STATE;
        }
        ret = write_cache_state(paths, state);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t load_cache_state_for_generation(
    const d1l_map_tile_provider_t *provider,
    const d1l_storage_status_t *storage,
    d1l_map_tile_cache_paths_t *paths,
    d1l_map_tile_cache_state_t **state,
    uint32_t backend_generation)
{
    if (!provider || !storage || !paths || !state ||
        !cache_control_paths(provider, paths)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cache_backend_generation_matches(backend_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * manager_attempt counts routine health polls, not mounted media changes.
     * The retained backend generation changes only across an SD availability
     * transition, so it invalidates this verified cache without forcing a
     * journal recovery scan for every tile.
     */
    if (s_cache_state_loaded &&
        strcmp(s_cache_state_source, provider->source_id) == 0 &&
        s_cache_state_capacity_kb == storage->capacity_kb &&
        s_cache_state_backend_generation == backend_generation) {
        *state = &s_cache_state;
        return ESP_OK;
    }

    d1l_map_tile_cache_state_t loaded = {0};
    d1l_rp2040_file_result_t state_file = {0};
    esp_err_t ret = d1l_rp2040_bridge_file_stat(
        paths->state, &state_file,
        D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (file_result_missing(ret, &state_file)) {
        d1l_map_tile_cache_state_init(&loaded);
    } else {
        if (ret != ESP_OK || !state_file.ok ||
            !state_file.exists || state_file.is_directory ||
            state_file.size != D1L_MAP_TILE_CACHE_STATE_BYTES) {
            return ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret;
        }
        uint8_t encoded[D1L_MAP_TILE_CACHE_STATE_BYTES];
        ret = read_file_exact(
            paths->state, 0U, encoded, sizeof(encoded));
        if (ret != ESP_OK ||
            !d1l_map_tile_cache_state_decode(
                encoded, &loaded)) {
            memset(encoded, 0, sizeof(encoded));
            return ret == ESP_OK ? ESP_ERR_INVALID_CRC : ret;
        }
        memset(encoded, 0, sizeof(encoded));
    }

    uint32_t journal_size = 0U;
    ret = repair_cache_journal(paths, &loaded, &journal_size);
    if (ret != ESP_OK || loaded.tail_offset > journal_size) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    ret = repair_cache_head(
        provider, paths, &loaded);
    if (ret == ESP_OK) {
        ret = recover_cache_tail(
            provider, paths, journal_size, &loaded);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (!cache_backend_generation_matches(backend_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cache_state = loaded;
    snprintf(s_cache_state_source,
             sizeof(s_cache_state_source), "%s",
             provider->source_id);
    s_cache_state_capacity_kb = storage->capacity_kb;
    s_cache_state_backend_generation = backend_generation;
    s_cache_state_loaded = true;
    *state = &s_cache_state;
    return ESP_OK;
}

static esp_err_t load_cache_state(
    const d1l_map_tile_provider_t *provider,
    const d1l_storage_status_t *storage,
    d1l_map_tile_cache_paths_t *paths,
    d1l_map_tile_cache_state_t **state)
{
    uint32_t backend_generation = 0U;
    const esp_err_t ret =
        cache_backend_generation(&backend_generation);
    return ret == ESP_OK ?
        load_cache_state_for_generation(
            provider, storage, paths, state, backend_generation) :
        ret;
}

static esp_err_t prepare_cache_room(
    const d1l_map_tile_provider_t *provider,
    const d1l_storage_status_t *storage,
    uint64_t required_bytes,
    d1l_map_tile_download_result_t *result,
    d1l_map_tile_continue_cb_t should_continue,
    void *continue_context)
{
    if (!provider || !storage || !result ||
        provider->cache_budget_mb == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_map_tile_cache_paths_t paths = {0};
    d1l_map_tile_cache_state_t *state = NULL;
    esp_err_t ret = load_cache_state(
        provider, storage, &paths, &state);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint64_t budget_bytes =
        (uint64_t)provider->cache_budget_mb * 1024ULL * 1024ULL;
    result->cache_budget_bytes = budget_bytes;
    if (required_bytes == 0U ||
        required_bytes > budget_bytes) {
        return ESP_ERR_NO_MEM;
    }
    while (!d1l_map_tile_cache_state_has_room(
               state, budget_bytes, required_bytes)) {
        /*
         * An eviction becomes one integrity unit at the first delete. Only
         * stop between fully committed eviction units.
         */
        if (!persistence_continue(
                result, should_continue, continue_context)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (state->head_offset >= state->tail_offset) {
            return ESP_ERR_NO_MEM;
        }
        d1l_map_tile_cache_record_t record = {0};
        ret = read_cache_record(
            paths.journal, state->head_offset, &record);
        if (ret != ESP_OK) {
            return ret;
        }
        d1l_map_tile_download_result_t oldest = {
            .z = record.zoom,
            .x = record.x,
            .y = record.y,
        };
        if (!tile_result_paths(provider, &oldest)) {
            return ESP_ERR_INVALID_ARG;
        }
        char *tile_path = oldest.path;
        char *metadata_path = oldest.metadata_path;
        d1l_map_tile_cache_record_t metadata = {0};
        const esp_err_t metadata_ret =
            read_cache_metadata(metadata_path, &metadata);
        if (metadata_ret == ESP_OK &&
            cache_records_equal(&record, &metadata)) {
            ret = d1l_rp2040_bridge_file_delete(tile_path,
                &result->file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
            if (!file_result_missing(ret, &result->file) &&
                ret != ESP_OK) {
                return ret;
            }
            ret = d1l_rp2040_bridge_file_delete(metadata_path,
                &result->file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
            if (!file_result_missing(ret, &result->file) &&
                ret != ESP_OK) {
                return ret;
            }
        } else if (metadata_ret != ESP_ERR_NOT_FOUND &&
                   metadata_ret != ESP_OK) {
            return metadata_ret;
        }
        (void)delete_file_allow_missing(oldest.tmp_path);
        (void)delete_file_allow_missing(oldest.metadata_tmp_path);
        if (!d1l_map_tile_cache_state_note_evict(
                state, &record)) {
            return ESP_ERR_INVALID_STATE;
        }
        ret = write_cache_state(&paths, state);
        if (ret != ESP_OK) {
            s_cache_state_loaded = false;
            return ret;
        }
        ++result->evicted_tiles;
    }
    result->cache_used_bytes = state->live_bytes;
    return ESP_OK;
}

static esp_err_t reconcile_cache_intent(
    const d1l_map_tile_provider_t *provider,
    const d1l_storage_status_t *storage,
    d1l_map_tile_download_result_t *result,
    const d1l_map_tile_cache_record_t *record,
    uint32_t original_tail_offset,
    esp_err_t original_error)
{
    if (!provider || !storage || !result || !record ||
        original_tail_offset >
            UINT32_MAX - D1L_MAP_TILE_CACHE_RECORD_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t committed_tail_offset =
        original_tail_offset + D1L_MAP_TILE_CACHE_RECORD_BYTES;
    s_cache_state_loaded = false;
    d1l_map_tile_cache_paths_t paths = {0};
    d1l_map_tile_cache_state_t *state = NULL;
    const esp_err_t ret = load_cache_state(
        provider, storage, &paths, &state);
    if (ret != ESP_OK || !state) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    if (state->tail_offset == original_tail_offset &&
        state->next_sequence == record->sequence) {
        result->cache_intent_recorded = false;
        return original_error;
    }
    if (state->tail_offset != committed_tail_offset ||
        state->next_sequence != cache_next_sequence(record->sequence)) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_map_tile_cache_record_t metadata = {0};
    const esp_err_t metadata_ret = read_cache_metadata(
        result->metadata_path, &metadata);
    const esp_err_t tile_ret = verify_tile_file(
        result->path, record);
    if (metadata_ret != ESP_OK ||
        !cache_records_equal(record, &metadata) ||
        tile_ret != ESP_OK) {
        return metadata_ret != ESP_OK ? metadata_ret : tile_ret;
    }
    result->rename_replace = true;
    result->cache_used_bytes = state->live_bytes;
    return ESP_OK;
}

static esp_err_t commit_cache_tile(
    const d1l_map_tile_provider_t *provider,
    const d1l_storage_status_t *storage,
    d1l_map_tile_download_result_t *result,
    d1l_map_tile_continue_cb_t should_continue,
    void *continue_context)
{
    if (!provider || !storage || !result ||
        result->bytes == 0U ||
        result->bytes > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!persistence_continue(
            result, should_continue, continue_context)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = prepare_cache_room(
        provider, storage, result->bytes, result,
        should_continue, continue_context);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!persistence_continue(
            result, should_continue, continue_context)) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_map_tile_cache_paths_t paths = {0};
    d1l_map_tile_cache_state_t *state = NULL;
    ret = load_cache_state(
        provider, storage, &paths, &state);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint64_t budget_bytes =
        (uint64_t)provider->cache_budget_mb * 1024ULL * 1024ULL;
    if (!d1l_map_tile_cache_state_has_room(
            state, budget_bytes, result->bytes)) {
        return ESP_ERR_NO_MEM;
    }
    if (state->tail_offset >
        UINT32_MAX - D1L_MAP_TILE_CACHE_RECORD_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t original_tail_offset = state->tail_offset;
    d1l_map_tile_cache_record_t record = {0};
    if (!d1l_map_tile_cache_record_init(
            state->next_sequence, result->z, result->x, result->y,
            (uint32_t)result->bytes, result->content_crc32,
            &record)) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = write_cache_metadata_tmp(result, &record);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!persistence_continue(
            result, should_continue, continue_context)) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * Once append is attempted, preserve both validated temporary files on
     * failure. The bridge reply can be lost after the durable append; the
     * next open decides from the checksummed journal instead of destroying
     * the only recoverable copy.
     */
    result->cache_intent_recorded = true;
    ret = append_cache_intent(&paths, state, &record);
    if (ret != ESP_OK) {
        return reconcile_cache_intent(
            provider, storage, result, &record,
            original_tail_offset, ret);
    }
    ret = d1l_rp2040_bridge_file_rename(
        result->tmp_path, result->path, true, &result->file,
        D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return reconcile_cache_intent(
            provider, storage, result, &record,
            original_tail_offset, ret);
    }
    result->rename_replace = true;
    ret = rename_cache_metadata(result);
    if (ret != ESP_OK) {
        return reconcile_cache_intent(
            provider, storage, result, &record,
            original_tail_offset, ret);
    }
    if (!d1l_map_tile_cache_state_note_commit(
            state, &record)) {
        return reconcile_cache_intent(
            provider, storage, result, &record,
            original_tail_offset, ESP_ERR_INVALID_STATE);
    }
    ret = write_cache_state(&paths, state);
    if (ret != ESP_OK) {
        return reconcile_cache_intent(
            provider, storage, result, &record,
            original_tail_offset, ret);
    }
    result->cache_used_bytes = state->live_bytes;
    return ESP_OK;
}

static bool attribution_metadata_present(
    const d1l_map_tile_download_result_t *result)
{
    if (!result || !result->attribution_path[0]) {
        return false;
    }
    d1l_rp2040_file_result_t file = {0};
    const esp_err_t ret = d1l_rp2040_bridge_file_stat(
        result->attribution_path, &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    return ret == ESP_OK && file.ok && file.exists && !file.is_directory && file.size > 0U;
}

static esp_err_t map_tile_store_cached_locked(
    uint8_t z,
    uint32_t x,
    uint32_t y,
    const d1l_storage_status_t *status,
    bool *out_cached)
{
    if (!status || !out_cached) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_cached = false;
    d1l_map_tile_provider_t provider = {0};
    d1l_map_tile_provider_snapshot(&provider);
    d1l_map_tile_download_result_t result = {0};
    init_download_result(&result, &provider, z, x, y, status, false);
    if (!result.sd_ready) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!result.path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t backend_generation = 0U;
    const esp_err_t backend_ret =
        cache_backend_generation(&backend_generation);
    if (backend_ret != ESP_OK) {
        return backend_ret;
    }
    d1l_map_tile_cache_record_t metadata = {0};
    const esp_err_t metadata_ret = read_cache_metadata(
        result.metadata_path, &metadata);
    if (metadata_ret != ESP_OK ||
        !cache_record_matches_tile(&metadata, z, x, y)) {
        return metadata_ret == ESP_OK ?
            ESP_ERR_INVALID_CRC : metadata_ret;
    }
    d1l_map_tile_cache_paths_t cache_paths = {0};
    d1l_map_tile_cache_state_t *cache_state = NULL;
    const esp_err_t cache_ret = load_cache_state_for_generation(
        &provider, status, &cache_paths, &cache_state,
        backend_generation);
    if (cache_ret != ESP_OK || !cache_state) {
        return cache_ret == ESP_OK ?
            ESP_ERR_INVALID_STATE : cache_ret;
    }
    d1l_rp2040_file_result_t file = {0};
    const esp_err_t ret = d1l_rp2040_bridge_file_stat(
        result.path, &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!file.ok || !file.exists || file.is_directory ||
        file.size != metadata.size ||
        file.size > D1L_MAP_TILE_DOWNLOAD_MAX_BYTES) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!provider.configured && !attribution_metadata_present(&result)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!cache_backend_generation_matches(backend_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_cached = true;
    return ESP_OK;
}

esp_err_t d1l_map_tile_store_cached(
    uint8_t z,
    uint32_t x,
    uint32_t y,
    const d1l_storage_status_t *status,
    bool *out_cached)
{
    if (out_cached) {
        *out_cached = false;
    }
    const esp_err_t lock_ret = cache_transaction_take();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }
    const esp_err_t ret = map_tile_store_cached_locked(
        z, x, y, status, out_cached);
    cache_transaction_give();
    return ret;
}

static esp_err_t map_tile_store_read_locked(
                                  uint8_t z,
                                  uint32_t x,
                                  uint32_t y,
                                  const d1l_storage_status_t *status,
                                  uint8_t *buffer,
                                  size_t buffer_size,
                                  size_t *out_len,
                                  d1l_map_tile_continue_cb_t should_continue,
                                  void *continue_context,
                                  d1l_map_tile_download_result_t *out_result)
{
    if (!buffer || !out_len || !out_result || buffer_size < D1L_MAP_TILE_DOWNLOAD_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    d1l_map_tile_provider_t provider = {0};
    d1l_map_tile_provider_snapshot(&provider);
    d1l_map_tile_download_result_t result;
    init_download_result(&result, &provider, z, x, y, status, false);
    if (!result.sd_ready || !d1l_map_tile_store_coord_valid(z, x, y) ||
        !result.path[0]) {
        download_step(&result, "preflight", ESP_ERR_NOT_SUPPORTED, NULL);
        *out_result = result;
        return result.last_error;
    }
    if (!continue_allowed(should_continue, continue_context)) {
        result.cancelled = true;
        download_step(&result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
        *out_result = result;
        return result.last_error;
    }
    uint32_t backend_generation = 0U;
    esp_err_t ret = cache_backend_generation(&backend_generation);
    if (ret != ESP_OK) {
        download_step(&result, "cache_backend", ret, NULL);
        *out_result = result;
        return result.last_error;
    }
    if (!provider.configured && !attribution_metadata_present(&result)) {
        download_step(&result, "metadata_missing", ESP_ERR_NOT_FOUND, NULL);
        *out_result = result;
        return result.last_error;
    }
    d1l_map_tile_cache_record_t metadata = {0};
    ret = read_cache_metadata(
        result.metadata_path, &metadata);
    if (ret != ESP_OK ||
        !cache_record_matches_tile(&metadata, z, x, y)) {
        download_step(
            &result, "cache_metadata",
            ret == ESP_OK ? ESP_ERR_INVALID_CRC : ret, NULL);
        *out_result = result;
        return result.last_error;
    }
    d1l_map_tile_cache_paths_t cache_paths = {0};
    d1l_map_tile_cache_state_t *cache_state = NULL;
    ret = load_cache_state_for_generation(
        &provider, status, &cache_paths, &cache_state,
        backend_generation);
    if (ret != ESP_OK || !cache_state) {
        download_step(
            &result, "cache_state",
            ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret, NULL);
        *out_result = result;
        return result.last_error;
    }

    d1l_rp2040_file_result_t file = {0};
    ret = d1l_rp2040_bridge_file_stat(result.path, &file,
                                      D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK || !file.ok || !file.exists || file.is_directory ||
        file.size != metadata.size ||
        file.size > D1L_MAP_TILE_DOWNLOAD_MAX_BYTES || file.size > buffer_size) {
        download_step(&result, "cache_miss", ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret, &file);
        *out_result = result;
        return result.last_error;
    }

    const uint32_t expected_size = file.size;
    bool saw_eof = false;
    while (result.bytes < expected_size) {
        if (!continue_allowed(should_continue, continue_context)) {
            result.cancelled = true;
            download_step(&result, "cancelled", ESP_ERR_INVALID_STATE, &file);
            *out_result = result;
            return result.last_error;
        }
        const size_t remaining = (size_t)expected_size - result.bytes;
        const size_t chunk = remaining < D1L_RP2040_FILE_CHUNK_MAX ?
                             remaining : D1L_RP2040_FILE_CHUNK_MAX;
        d1l_rp2040_file_result_t read_result = {0};
        ret = d1l_rp2040_bridge_file_read(result.path, (uint32_t)result.bytes,
                                          &buffer[result.bytes], chunk, &read_result,
                                          D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        if (ret != ESP_OK || !read_result.ok ||
            read_result.offset != (uint32_t)result.bytes ||
            read_result.length == 0U || read_result.length > chunk ||
            read_result.eof !=
                (result.bytes + read_result.length == (size_t)expected_size)) {
            download_step(&result, "cache_read", ret == ESP_OK ? ESP_FAIL : ret,
                          &read_result);
            *out_result = result;
            return result.last_error;
        }
        result.bytes += read_result.length;
        saw_eof = read_result.eof;
        file = read_result;
    }
    if (result.bytes != (size_t)expected_size || !saw_eof) {
        download_step(&result, "cache_read", ESP_FAIL, &file);
        *out_result = result;
        return result.last_error;
    }
    result.png_valid = d1l_map_tile_png_valid(buffer, result.bytes);
    if (!result.png_valid) {
        download_step(&result, "cache_png", ESP_ERR_INVALID_RESPONSE, &file);
        *out_result = result;
        return result.last_error;
    }
    result.content_crc32 = d1l_map_tile_cache_crc32(
        buffer, result.bytes);
    result.checksum_verified =
        result.content_crc32 == metadata.content_crc32;
    if (!result.checksum_verified) {
        download_step(
            &result, "cache_checksum",
            ESP_ERR_INVALID_CRC, &file);
        *out_result = result;
        return result.last_error;
    }
    if (!cache_backend_generation_matches(backend_generation)) {
        download_step(
            &result, "cache_backend",
            ESP_ERR_INVALID_STATE, &file);
        *out_result = result;
        return result.last_error;
    }
    result.cache_hit = true;
    result.attribution_saved = true;
    download_step(&result, "cache_hit", ESP_OK, &file);
    *out_len = result.bytes;
    *out_result = result;
    return ESP_OK;
}

esp_err_t d1l_map_tile_store_read(uint8_t z,
                                  uint32_t x,
                                  uint32_t y,
                                  const d1l_storage_status_t *status,
                                  uint8_t *buffer,
                                  size_t buffer_size,
                                  size_t *out_len,
                                  d1l_map_tile_continue_cb_t should_continue,
                                  void *continue_context,
                                  d1l_map_tile_download_result_t *out_result)
{
    const esp_err_t lock_ret = cache_transaction_take();
    if (lock_ret != ESP_OK) {
        if (out_len) {
            *out_len = 0U;
        }
        if (out_result) {
            memset(out_result, 0, sizeof(*out_result));
            out_result->last_error = lock_ret;
        }
        return lock_ret;
    }
    const esp_err_t ret = map_tile_store_read_locked(
        z, x, y, status, buffer, buffer_size, out_len,
        should_continue, continue_context, out_result);
    cache_transaction_give();
    return ret;
}

typedef struct {
    char content_type[32];
    uint32_t retry_after_sec;
    uint32_t minimum_request_interval_ms;
    uint32_t background_token;
    int socket_fd;
    char source_id[D1L_MAP_PROVIDER_SOURCE_ID_MAX + 1U];
} map_http_context_t;

static esp_err_t map_http_event(esp_http_client_event_t *event)
{
    if (!event || !event->user_data) {
        return ESP_OK;
    }
    map_http_context_t *context =
        (map_http_context_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        context->socket_fd =
            esp_http_client_get_socket(event->client);
        background_fetch_publish_socket(
            context->background_token, context->socket_fd);
        return ESP_OK;
    }
    if (event->event_id == HTTP_EVENT_HEADERS_SENT) {
        request_gate_extend_minimum(
            context->source_id,
            esp_timer_get_time() +
                (int64_t)context->minimum_request_interval_ms *
                    1000LL);
        return ESP_OK;
    }
    if (event->event_id == HTTP_EVENT_DISCONNECTED) {
        background_fetch_clear_socket(
            context->background_token, context->socket_fd);
        context->socket_fd = -1;
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_HEADER ||
        !event->header_key || !event->header_value) {
        return ESP_OK;
    }
    if (strcasecmp(event->header_key, "Content-Type") == 0) {
        snprintf(context->content_type, sizeof(context->content_type), "%s",
                 event->header_value);
    } else if (strcasecmp(event->header_key, "Retry-After") == 0) {
        char *end = NULL;
        const unsigned long value = strtoul(event->header_value, &end, 10);
        if (end != event->header_value && value <= 86400UL) {
            context->retry_after_sec = (uint32_t)value;
        }
    }
    return ESP_OK;
}

static bool png_content_type(const char *content_type)
{
    static const char expected[] = "image/png";
    return content_type && strncasecmp(content_type, expected, sizeof(expected) - 1U) == 0 &&
           (content_type[sizeof(expected) - 1U] == '\0' ||
            content_type[sizeof(expected) - 1U] == ';' ||
            content_type[sizeof(expected) - 1U] == ' ');
}

static bool cache_metadata_matches_candidate(
    const d1l_map_tile_cache_record_t *metadata,
    const d1l_map_tile_download_result_t *result)
{
    return metadata && result &&
           cache_record_matches_tile(
               metadata, result->z, result->x, result->y) &&
           metadata->size == result->bytes &&
           metadata->content_crc32 == result->content_crc32;
}

static esp_err_t persist_validated_tile(
    const d1l_map_tile_provider_t *provider,
    const d1l_storage_status_t *storage,
    const uint8_t *buffer,
    d1l_map_tile_download_result_t *result,
    d1l_map_tile_continue_cb_t should_continue,
    void *continue_context)
{
    if (!provider || !storage || !buffer || !result ||
        result->bytes == 0U || !result->png_valid) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = cache_transaction_take_cancelable(
        result, should_continue, continue_context);
    if (ret != ESP_OK) {
        return ret;
    }
    bool candidate_tmp_owned = false;
    d1l_map_tile_cache_paths_t paths = {0};
    d1l_map_tile_cache_state_t *state = NULL;
    ret = load_cache_state(
        provider, storage, &paths, &state);
    if (ret != ESP_OK || !state) {
        ret = ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
        goto persist_done;
    }
    if (!persistence_continue(
            result, should_continue, continue_context)) {
        ret = ESP_ERR_INVALID_STATE;
        goto persist_done;
    }
    result->cache_budget_bytes =
        (uint64_t)provider->cache_budget_mb * 1024ULL * 1024ULL;
    result->cache_used_bytes = state->live_bytes;

    /*
     * Another network worker may have committed the same response while this
     * worker was outside the mutex. Reuse that exact checksummed tile rather
     * than disturbing its files or consuming a second journal sequence.
     */
    d1l_map_tile_cache_record_t metadata = {0};
    const esp_err_t metadata_ret = read_cache_metadata(
        result->metadata_path, &metadata);
    const esp_err_t existing_tile_ret =
        metadata_ret == ESP_OK &&
        cache_metadata_matches_candidate(
            &metadata, result) ?
            verify_tile_file_continue(
                result->path, &metadata, result,
                should_continue, continue_context) :
            ESP_ERR_NOT_FOUND;
    if (result->cancelled) {
        ret = ESP_ERR_INVALID_STATE;
        goto persist_done;
    }
    if (metadata_ret == ESP_OK &&
        cache_metadata_matches_candidate(&metadata, result) &&
        existing_tile_ret == ESP_OK) {
        result->cache_hit = true;
        result->checksum_verified = true;
        result->attribution_saved = true;
        ret = ESP_OK;
        goto persist_done;
    }

    if (!persistence_continue(
            result, should_continue, continue_context)) {
        ret = ESP_ERR_INVALID_STATE;
        goto persist_done;
    }
    cleanup_partial(result);
    candidate_tmp_owned = true;
    result->checksum_verified = false;
    d1l_rp2040_file_result_t file = {0};
    for (size_t offset = 0U; offset < result->bytes;) {
        if (!persistence_continue(
                result, should_continue, continue_context)) {
            ret = ESP_ERR_INVALID_STATE;
            goto persist_done;
        }
        const size_t remaining = result->bytes - offset;
        const size_t chunk =
            remaining < D1L_RP2040_FILE_CHUNK_MAX ?
                remaining : D1L_RP2040_FILE_CHUNK_MAX;
        ret = d1l_rp2040_bridge_file_write(
            result->tmp_path, (uint32_t)offset,
            &buffer[offset], chunk, offset == 0U,
            &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        if (ret != ESP_OK ||
            file.offset != offset ||
            file.length != chunk ||
            file.size != offset + chunk) {
            ret = ret == ESP_OK ? ESP_FAIL : ret;
            goto persist_done;
        }
        result->write_tmp = true;
        offset += chunk;
    }
    if (!persistence_continue(
            result, should_continue, continue_context)) {
        ret = ESP_ERR_INVALID_STATE;
        goto persist_done;
    }
    d1l_map_tile_cache_record_t verify_record = {0};
    if (!d1l_map_tile_cache_record_init(
            1U, result->z, result->x, result->y,
            (uint32_t)result->bytes, result->content_crc32,
            &verify_record)) {
        ret = ESP_ERR_INVALID_STATE;
        goto persist_done;
    }
    ret = verify_tile_file_continue(
        result->tmp_path, &verify_record, result,
        should_continue, continue_context);
    if (ret != ESP_OK) {
        goto persist_done;
    }
    result->checksum_verified = true;
    if (!persistence_continue(
            result, should_continue, continue_context)) {
        ret = ESP_ERR_INVALID_STATE;
        goto persist_done;
    }
    ret = write_attribution_metadata(
        provider, result,
        should_continue, continue_context);
    if (ret == ESP_OK &&
        persistence_continue(
            result, should_continue, continue_context)) {
        ret = commit_cache_tile(
            provider, storage, result,
            should_continue, continue_context);
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_STATE;
    }

persist_done:
    if (ret != ESP_OK && candidate_tmp_owned &&
        !result->cache_intent_recorded &&
        !result->cancelled) {
        cleanup_partial(result);
    }
    cache_transaction_give();
    return ret;
}

static esp_err_t map_tile_store_fetch_network(
                                   uint8_t z,
                                   uint32_t x,
                                   uint32_t y,
                                   const d1l_storage_status_t *status,
                                   bool wifi_connected,
                                   uint32_t request_timeout_ms,
                                   bool background_fetch,
                                   uint8_t *buffer,
                                   size_t buffer_size,
                                   size_t *out_len,
                                   d1l_map_tile_continue_cb_t should_continue,
                                   void *continue_context,
                                   d1l_map_tile_download_result_t *out_result)
{
    if (!buffer || !out_len || !out_result || request_timeout_ms == 0U ||
        buffer_size < D1L_MAP_TILE_DOWNLOAD_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    d1l_map_tile_provider_t provider = {0};
    d1l_map_tile_provider_snapshot(&provider);
    d1l_map_tile_download_result_t result;
    init_download_result(
        &result, &provider, z, x, y, status, wifi_connected);
    if (!d1l_map_tile_store_coord_valid(z, x, y) || !result.path[0]) {
        download_step(&result, "coordinate", ESP_ERR_INVALID_ARG, NULL);
        *out_result = result;
        return result.last_error;
    }
    if (!provider.network_fetch_allowed) {
        download_step(&result, "provider_preloaded_only",
                      ESP_ERR_NOT_SUPPORTED, NULL);
        *out_result = result;
        return result.last_error;
    }
    if (!build_tile_url(provider.url_template, z, x, y,
                        result.url, sizeof(result.url))) {
        download_step(&result, "provider_config",
                      ESP_ERR_INVALID_RESPONSE, NULL);
        *out_result = result;
        return result.last_error;
    }
    if (!wifi_connected) {
        download_step(&result, "wifi", ESP_ERR_INVALID_STATE, NULL);
        *out_result = result;
        return result.last_error;
    }
    if (!result.sd_ready) {
        download_step(&result, "sd_cache_required", ESP_ERR_NOT_SUPPORTED, NULL);
        *out_result = result;
        return result.last_error;
    }
    d1l_map_network_continue_t continuation = {
        .caller = should_continue,
        .caller_context = continue_context,
        .background_token = 0U,
    };
    if (!map_network_continue(&continuation)) {
        result.cancelled = true;
        download_step(&result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
        *out_result = result;
        return result.last_error;
    }
    if (background_fetch) {
        continuation.background_token =
            background_fetch_begin();
        if (continuation.background_token == 0U) {
            download_step(
                &result, "cancel_registry", ESP_ERR_NO_MEM, NULL);
            *out_result = result;
            return result.last_error;
        }
        if (!map_network_continue(&continuation)) {
            result.cancelled = true;
            download_step(
                &result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
            background_fetch_finish(
                continuation.background_token);
            *out_result = result;
            return result.last_error;
        }
    }
    esp_err_t ret = d1l_connectivity_network_lease_begin(
        D1L_MAP_TILE_NETWORK_LEASE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        result.cancelled = !map_network_continue(&continuation);
        download_step(
            &result, result.cancelled ? "cancelled" : "network_lease",
            ret, NULL);
        background_fetch_finish(
            continuation.background_token);
        *out_result = result;
        return result.last_error;
    }
    esp_http_client_handle_t client = NULL;
    bool persist_tile = false;

    ret = d1l_time_service_wait_for_certificate_time(
        D1L_TIME_TLS_WAIT_TIMEOUT_MS, D1L_TIME_TLS_WAIT_SLICE_MS,
        map_network_continue, &continuation);
    if (ret != ESP_OK) {
        if (!map_network_continue(&continuation)) {
            result.cancelled = true;
            download_step(&result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
        } else {
            download_step(&result, "time_sync", ret, NULL);
        }
        goto network_done;
    }

    int gate_retry_status = 0;
    uint32_t gate_retry_after_sec = 0U;
    ret = request_gate_wait(
        provider.source_id,
        map_network_continue, &continuation,
        &gate_retry_status, &gate_retry_after_sec);
    if (ret != ESP_OK) {
        if (gate_retry_status == 429 ||
            gate_retry_status == 503) {
            result.status_code = gate_retry_status;
            result.retry_after_sec =
                gate_retry_after_sec;
            download_step(
                &result, "rate_limited",
                ESP_ERR_TIMEOUT, NULL);
        } else if (!map_network_continue(&continuation)) {
            result.cancelled = true;
            download_step(
                &result, "cancelled",
                ESP_ERR_INVALID_STATE, NULL);
        } else {
            download_step(
                &result, "request_gate", ret, NULL);
        }
        goto network_done;
    }

    map_http_context_t http_context = {
        .minimum_request_interval_ms =
            provider.minimum_request_interval_ms,
        .background_token =
            continuation.background_token,
        .socket_fd = -1,
    };
    snprintf(
        http_context.source_id,
        sizeof(http_context.source_id), "%s",
        provider.source_id);
    esp_http_client_config_t config = {
        .url = result.url,
        .method = HTTP_METHOD_GET,
        /*
         * The provider contract requires HTTPS, so IDF's async transport is
         * available. Keep each socket/TLS wait on this owning task short; the
         * phase deadlines below supply the foreground/background budget.
         */
        .timeout_ms = D1L_MAP_TILE_HTTP_IO_SLICE_MS,
        .is_async = true,
        .user_agent = D1L_MAP_TILE_USER_AGENT,
        .buffer_size = D1L_RP2040_FILE_CHUNK_MAX,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = map_http_event,
        .user_data = &http_context,
    };
    client = esp_http_client_init(&config);
    if (!client) {
        download_step(&result, "http_init", ESP_FAIL, NULL);
        goto network_done;
    }

    const int64_t open_deadline_us = esp_timer_get_time() +
        (int64_t)request_timeout_ms * 1000LL;
    do {
        if (!map_network_continue(&continuation)) {
            result.cancelled = true;
            download_step(
                &result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
            goto network_done;
        }
        ret = esp_http_client_open(client, 0);
        if (ret == ESP_ERR_HTTP_EAGAIN &&
            esp_timer_get_time() >= open_deadline_us) {
            download_step(&result, "http_open", ESP_ERR_TIMEOUT, NULL);
            ret = result.last_error;
            goto network_done;
        }
        if (ret == ESP_ERR_HTTP_EAGAIN) {
            vTaskDelay(1U);
        }
    } while (ret == ESP_ERR_HTTP_EAGAIN);
    if (ret != ESP_OK) {
        download_step(&result, "http_open", ret, NULL);
        goto network_done;
    }
    if (!map_network_continue(&continuation)) {
        result.cancelled = true;
        download_step(&result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
        goto network_done;
    }
    const int64_t header_deadline_us = esp_timer_get_time() +
        (int64_t)request_timeout_ms * 1000LL;
    int64_t content_length = -ESP_ERR_HTTP_EAGAIN;
    while (content_length == -ESP_ERR_HTTP_EAGAIN) {
        if (!map_network_continue(&continuation)) {
            result.cancelled = true;
            download_step(
                &result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
            goto network_done;
        }
        content_length = esp_http_client_fetch_headers(client);
        if (content_length == -ESP_ERR_HTTP_EAGAIN &&
            esp_timer_get_time() >= header_deadline_us) {
            download_step(
                &result, "fetch_headers", ESP_ERR_TIMEOUT, NULL);
            ret = result.last_error;
            goto network_done;
        }
        if (content_length == -ESP_ERR_HTTP_EAGAIN) {
            vTaskDelay(1U);
        }
    }
    if (content_length >= 0) {
        result.status_code =
            esp_http_client_get_status_code(client);
        result.retry_after_sec =
            http_context.retry_after_sec;
        result.content_type_valid =
            png_content_type(http_context.content_type);
        if (result.status_code == 429 ||
            result.status_code == 503) {
            if (result.retry_after_sec == 0U) {
                result.retry_after_sec =
                    D1L_MAP_TILE_DEFAULT_RETRY_AFTER_SEC;
            }
            request_gate_extend_retry(
                provider.source_id,
                result.status_code,
                esp_timer_get_time() +
                    (int64_t)result.retry_after_sec *
                        1000000LL);
            download_step(
                &result, "rate_limited",
                ESP_ERR_TIMEOUT, NULL);
            ret = result.last_error;
            goto network_done;
        }
    }
    if (!map_network_continue(&continuation)) {
        result.cancelled = true;
        download_step(&result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
        goto network_done;
    }
    if (content_length < 0) {
        download_step(&result, "fetch_headers", ESP_FAIL, NULL);
        ret = result.last_error;
        goto network_done;
    }
    const bool chunked = esp_http_client_is_chunked_response(client);
    /* ESP-IDF reports zero both for chunked/non-positive responses.  Only a
     * positive, non-chunked value is an exact Content-Length contract. */
    const bool content_length_known = !chunked && content_length > 0;
    const size_t download_limit = buffer_size < D1L_MAP_TILE_DOWNLOAD_MAX_BYTES ?
                                  buffer_size : D1L_MAP_TILE_DOWNLOAD_MAX_BYTES;
    if (result.status_code != 200) {
        download_step(&result, "http_status", ESP_FAIL, NULL);
        ret = result.last_error;
        goto network_done;
    }
    if (!result.content_type_valid) {
        download_step(&result, "content_type", ESP_ERR_INVALID_RESPONSE, NULL);
        ret = result.last_error;
        goto network_done;
    }
    if (content_length_known && content_length > (int64_t)download_limit) {
        download_step(&result, "content_length", ESP_ERR_INVALID_SIZE, NULL);
        ret = result.last_error;
        goto network_done;
    }

    int idle_reads = 0;
    int64_t read_deadline_us = esp_timer_get_time() +
        (int64_t)request_timeout_ms * 1000LL;
    while (!esp_http_client_is_complete_data_received(client)) {
        if (!map_network_continue(&continuation)) {
            result.cancelled = true;
            download_step(&result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
            ret = result.last_error;
            goto network_done;
        }
        const size_t remaining = download_limit - result.bytes;
        const size_t want = remaining < D1L_RP2040_FILE_CHUNK_MAX ?
                            remaining : D1L_RP2040_FILE_CHUNK_MAX;
        if (want == 0U) {
            download_step(&result, "too_large", ESP_ERR_INVALID_SIZE, NULL);
            ret = result.last_error;
            goto network_done;
        }
        const int read_len = esp_http_client_read(client, (char *)&buffer[result.bytes], want);
        if (read_len == -ESP_ERR_HTTP_EAGAIN) {
            if (!map_network_continue(&continuation)) {
                result.cancelled = true;
                download_step(
                    &result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
                ret = result.last_error;
                goto network_done;
            }
            if (esp_timer_get_time() >= read_deadline_us) {
                download_step(
                    &result, "http_read", ESP_ERR_TIMEOUT, NULL);
                ret = result.last_error;
                goto network_done;
            }
            vTaskDelay(1U);
            continue;
        }
        if (read_len < 0) {
            download_step(&result, "http_read", ESP_FAIL, NULL);
            ret = result.last_error;
            goto network_done;
        }
        if ((size_t)read_len > want) {
            download_step(&result, "too_large", ESP_ERR_INVALID_SIZE, NULL);
            ret = result.last_error;
            goto network_done;
        }
        if (read_len == 0) {
            if (++idle_reads > 3) {
                break;
            }
            continue;
        }
        idle_reads = 0;
        result.bytes += (size_t)read_len;
        read_deadline_us = esp_timer_get_time() +
            (int64_t)request_timeout_ms * 1000LL;
    }
    if (!esp_http_client_is_complete_data_received(client) || result.bytes == 0U ||
        (content_length_known && result.bytes != (size_t)content_length)) {
        download_step(&result, "http_incomplete", ESP_FAIL, NULL);
        ret = result.last_error;
        goto network_done;
    }
    result.png_valid = d1l_map_tile_png_valid(buffer, result.bytes);
    if (!result.png_valid) {
        download_step(&result, "png", ESP_ERR_INVALID_RESPONSE, NULL);
        ret = result.last_error;
        goto network_done;
    }
    result.content_crc32 = d1l_map_tile_cache_crc32(
        buffer, result.bytes);
    if (!map_network_continue(&continuation)) {
        result.cancelled = true;
        download_step(
            &result, "cancelled",
            ESP_ERR_INVALID_STATE, NULL);
        ret = result.last_error;
        goto network_done;
    }
    persist_tile = true;

network_done:
    const bool rate_response =
        result.status_code == 429 ||
        result.status_code == 503;
    if (!persist_tile && !rate_response &&
        (!map_network_continue(&continuation) ||
         background_fetch_cancel_requested(
             continuation.background_token))) {
        result.cancelled = true;
        download_step(
            &result, "cancelled",
            ESP_ERR_INVALID_STATE, NULL);
        ret = result.last_error;
    }
    /*
     * Detach the descriptor under the token mutex before the owner closes it.
     * Keep only the cancellation latch active through reversible persistence.
     */
    background_fetch_detach_socket(
        continuation.background_token);
    if (client) {
        (void)esp_http_client_close(client);
        (void)esp_http_client_cleanup(client);
    }
    d1l_connectivity_network_lease_end();
    if (persist_tile) {
        ret = persist_validated_tile(
            &provider, status, buffer, &result,
            map_network_continue, &continuation);
        if (ret != ESP_OK) {
            download_step(
                &result,
                result.cancelled ?
                    "cancelled" : "cache_persist",
                result.cancelled ?
                    ESP_ERR_INVALID_STATE : ret,
                &result.file);
        } else {
            download_step(&result, "ok", ESP_OK, &result.file);
            *out_len = result.bytes;
        }
    }
    background_fetch_finish(
        continuation.background_token);
    *out_result = result;
    return result.last_error;
}

esp_err_t d1l_map_tile_store_fetch(uint8_t z,
                                   uint32_t x,
                                   uint32_t y,
                                   const d1l_storage_status_t *status,
                                   bool wifi_connected,
                                   uint8_t *buffer,
                                   size_t buffer_size,
                                   size_t *out_len,
                                   d1l_map_tile_continue_cb_t should_continue,
                                   void *continue_context,
                                   d1l_map_tile_download_result_t *out_result)
{
    return map_tile_store_fetch_network(
        z, x, y, status, wifi_connected, D1L_MAP_TILE_HTTP_TIMEOUT_MS,
        false, buffer, buffer_size,
        out_len, should_continue, continue_context, out_result);
}

esp_err_t d1l_map_tile_store_fetch_background(
    uint8_t z,
    uint32_t x,
    uint32_t y,
    const d1l_storage_status_t *status,
    bool wifi_connected,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *out_len,
    d1l_map_tile_continue_cb_t should_continue,
    void *continue_context,
    d1l_map_tile_download_result_t *out_result)
{
    return map_tile_store_fetch_network(
        z, x, y, status, wifi_connected,
        D1L_MAP_TILE_BACKGROUND_HTTP_TIMEOUT_MS, true,
        buffer, buffer_size,
        out_len, should_continue, continue_context, out_result);
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
esp_err_t d1l_map_tile_store_write_canary(const char *token,
                                          const d1l_storage_status_t *status,
                                          d1l_map_tile_canary_result_t *out_result)
{
    if (!token || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }

    d1l_map_tile_canary_result_t result = {
        .public_rf_tx = false,
        .formats_sd = false,
        .last_error = ESP_OK,
    };
    if (!d1l_map_tile_store_token_valid(token)) {
        result_step(&result, "token", ESP_ERR_INVALID_ARG, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(result.token, sizeof(result.token), "%s", token);
    if (!result_path_set(&result, token)) {
        result_step(&result, "path", ESP_ERR_INVALID_SIZE, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_SIZE;
    }
    if (!d1l_map_tile_store_sd_ready(status)) {
        result_step(&result, "preflight", ESP_ERR_NOT_SUPPORTED, NULL);
        *out_result = result;
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t payload[D1L_RP2040_FILE_CHUNK_MAX];
    size_t payload_len = 0;
    esp_err_t payload_ret = build_canary_payload(token, payload, sizeof(payload), &payload_len);
    if (payload_ret != ESP_OK) {
        result_step(&result, "payload", payload_ret, NULL);
        *out_result = result;
        return payload_ret;
    }
    result.bytes = payload_len;

    uint8_t read_buf[D1L_RP2040_FILE_CHUNK_MAX] = {0};
    d1l_rp2040_file_result_t file = {0};
    esp_err_t ret = d1l_rp2040_bridge_file_delete(result.tmp_path, &file,
                                                  D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND && strcmp(file.err, "not_found") != 0) {
        result_step(&result, "cleanup_tmp", ret, &file);
        *out_result = result;
        return ret;
    }

    ret = d1l_rp2040_bridge_file_write(result.tmp_path, 0U, payload,
                                       (size_t)payload_len, true, &file,
                                       D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK || file.length != (uint32_t)payload_len ||
        file.size != (uint32_t)payload_len) {
        result_step(&result, "write_tmp", ret == ESP_OK ? ESP_FAIL : ret, &file);
        *out_result = result;
        return result.last_error;
    }
    result.write_tmp = true;

    ret = d1l_rp2040_bridge_file_read(result.tmp_path, 0U, read_buf,
                                      (size_t)payload_len, &file,
                                      D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK || file.length != (uint32_t)payload_len ||
        memcmp(read_buf, payload, (size_t)payload_len) != 0) {
        result_step(&result, "read_tmp", ret == ESP_OK ? ESP_FAIL : ret, &file);
        *out_result = result;
        return result.last_error;
    }
    result.read_tmp = true;

    ret = d1l_rp2040_bridge_file_rename(result.tmp_path, result.path, true,
                                        &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        result_step(&result, "rename_final", ret, &file);
        *out_result = result;
        return ret;
    }
    result.rename_replace = true;

    ret = d1l_rp2040_bridge_file_stat(result.path, &file,
                                      D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK || !file.exists || file.is_directory ||
        file.size != (uint32_t)payload_len) {
        result_step(&result, "stat_final", ret == ESP_OK ? ESP_FAIL : ret, &file);
        *out_result = result;
        return result.last_error;
    }
    result.stat_final = true;

    memset(read_buf, 0, sizeof(read_buf));
    ret = d1l_rp2040_bridge_file_read(result.path, 0U, read_buf,
                                      (size_t)payload_len, &file,
                                      D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK || file.length != (uint32_t)payload_len ||
        memcmp(read_buf, payload, (size_t)payload_len) != 0) {
        result_step(&result, "read_final", ret == ESP_OK ? ESP_FAIL : ret, &file);
        *out_result = result;
        return result.last_error;
    }
    result.read_final = true;
    result_step(&result, "ok", ESP_OK, &file);
    *out_result = result;
    return ESP_OK;
}

esp_err_t d1l_map_tile_store_check_canary(const char *token,
                                          const d1l_storage_status_t *status,
                                          d1l_map_tile_canary_result_t *out_result)
{
    if (!token || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }

    d1l_map_tile_canary_result_t result = {
        .public_rf_tx = false,
        .formats_sd = false,
        .last_error = ESP_OK,
    };
    if (!d1l_map_tile_store_token_valid(token)) {
        result_step(&result, "token", ESP_ERR_INVALID_ARG, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(result.token, sizeof(result.token), "%s", token);
    if (!result_path_set(&result, token)) {
        result_step(&result, "path", ESP_ERR_INVALID_SIZE, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_SIZE;
    }
    if (!d1l_map_tile_store_sd_ready(status)) {
        result_step(&result, "preflight", ESP_ERR_NOT_SUPPORTED, NULL);
        *out_result = result;
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t payload[D1L_RP2040_FILE_CHUNK_MAX];
    size_t payload_len = 0;
    esp_err_t payload_ret = build_canary_payload(token, payload, sizeof(payload), &payload_len);
    if (payload_ret != ESP_OK) {
        result_step(&result, "payload", payload_ret, NULL);
        *out_result = result;
        return payload_ret;
    }
    result.bytes = payload_len;

    d1l_rp2040_file_result_t file = {0};
    esp_err_t ret = d1l_rp2040_bridge_file_stat(result.path, &file,
                                                D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK || !file.exists || file.is_directory ||
        file.size != (uint32_t)payload_len) {
        result_step(&result, "stat_final", ret == ESP_OK ? ESP_FAIL : ret, &file);
        *out_result = result;
        return result.last_error;
    }
    result.stat_final = true;

    uint8_t read_buf[D1L_RP2040_FILE_CHUNK_MAX] = {0};
    ret = d1l_rp2040_bridge_file_read(result.path, 0U, read_buf, payload_len,
                                      &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK || file.length != (uint32_t)payload_len ||
        memcmp(read_buf, payload, payload_len) != 0) {
        result_step(&result, "read_final", ret == ESP_OK ? ESP_FAIL : ret, &file);
        *out_result = result;
        return result.last_error;
    }
    result.read_final = true;
    result_step(&result, "ok", ESP_OK, &file);
    *out_result = result;
    return ESP_OK;
}
#endif
