#include "map_tile_store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "map/map_tile_provider.h"
#include "platform/time_service.h"
#include "storage/storage_status_policy.h"

#define D1L_MAP_TILE_CANARY_Z 12U
#define D1L_MAP_TILE_CANARY_X 1U
#define D1L_MAP_TILE_CANARY_Y 2U
#define D1L_MAP_TILE_HTTP_TIMEOUT_MS 15000
#define D1L_MAP_TILE_SD_FILE_TIMEOUT_MS 10000U

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
        path_length + 1U >= sizeof(result->tmp_path)) {
        return false;
    }
    memcpy(result->tmp_path, result->path, path_length - 4U);
    memcpy(&result->tmp_path[path_length - 4U], ".tmp", 5U);
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
    d1l_map_tile_download_result_t *result)
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
    d1l_rp2040_file_result_t file = {0};
    (void)d1l_rp2040_bridge_file_delete(result->attribution_tmp_path, &file,
                                        D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    size_t offset = 0U;
    while (offset < (size_t)len) {
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

esp_err_t d1l_map_tile_store_cached(
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
    d1l_rp2040_file_result_t file = {0};
    const esp_err_t ret = d1l_rp2040_bridge_file_stat(
        result.path, &file, D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!file.ok || !file.exists || file.is_directory || file.size == 0U ||
        file.size > D1L_MAP_TILE_DOWNLOAD_MAX_BYTES) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!provider.configured && !attribution_metadata_present(&result)) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_cached = true;
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
    if (!provider.configured && !attribution_metadata_present(&result)) {
        download_step(&result, "metadata_missing", ESP_ERR_NOT_FOUND, NULL);
        *out_result = result;
        return result.last_error;
    }

    d1l_rp2040_file_result_t file = {0};
    esp_err_t ret = d1l_rp2040_bridge_file_stat(result.path, &file,
                                                D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK || !file.ok || !file.exists || file.is_directory || file.size == 0U ||
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
    result.cache_hit = true;
    result.attribution_saved = true;
    download_step(&result, "cache_hit", ESP_OK, &file);
    *out_len = result.bytes;
    *out_result = result;
    return ESP_OK;
}

typedef struct {
    char content_type[32];
    uint32_t retry_after_sec;
} map_http_headers_t;

static esp_err_t map_http_event(esp_http_client_event_t *event)
{
    if (!event || event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data ||
        !event->header_key || !event->header_value) {
        return ESP_OK;
    }
    map_http_headers_t *headers = (map_http_headers_t *)event->user_data;
    if (strcasecmp(event->header_key, "Content-Type") == 0) {
        snprintf(headers->content_type, sizeof(headers->content_type), "%s",
                 event->header_value);
    } else if (strcasecmp(event->header_key, "Retry-After") == 0) {
        char *end = NULL;
        const unsigned long value = strtoul(event->header_value, &end, 10);
        if (end != event->header_value && value <= 86400UL) {
            headers->retry_after_sec = (uint32_t)value;
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
    if (!buffer || !out_len || !out_result || buffer_size < D1L_MAP_TILE_DOWNLOAD_MAX_BYTES) {
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
    if (!continue_allowed(should_continue, continue_context)) {
        result.cancelled = true;
        download_step(&result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
        *out_result = result;
        return result.last_error;
    }
    esp_err_t ret = d1l_time_service_wait_for_certificate_time(
        D1L_TIME_TLS_WAIT_TIMEOUT_MS, D1L_TIME_TLS_WAIT_SLICE_MS,
        should_continue, continue_context);
    if (ret != ESP_OK) {
        if (!continue_allowed(should_continue, continue_context)) {
            result.cancelled = true;
            download_step(&result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
        } else {
            download_step(&result, "time_sync", ret, NULL);
        }
        *out_result = result;
        return result.last_error;
    }

    map_http_headers_t headers = {0};
    esp_http_client_config_t config = {
        .url = result.url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = D1L_MAP_TILE_HTTP_TIMEOUT_MS,
        .user_agent = D1L_MAP_TILE_USER_AGENT,
        .buffer_size = D1L_RP2040_FILE_CHUNK_MAX,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = map_http_event,
        .user_data = &headers,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        download_step(&result, "http_init", ESP_FAIL, NULL);
        *out_result = result;
        return result.last_error;
    }

    d1l_rp2040_file_result_t file = {0};
    cleanup_partial(&result);
    ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        download_step(&result, "http_open", ret, NULL);
        goto fetch_done;
    }
    const int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        download_step(&result, "fetch_headers", ESP_FAIL, NULL);
        ret = result.last_error;
        goto fetch_done;
    }
    const bool chunked = esp_http_client_is_chunked_response(client);
    /* ESP-IDF reports zero both for chunked/non-positive responses.  Only a
     * positive, non-chunked value is an exact Content-Length contract. */
    const bool content_length_known = !chunked && content_length > 0;
    const size_t download_limit = buffer_size < D1L_MAP_TILE_DOWNLOAD_MAX_BYTES ?
                                  buffer_size : D1L_MAP_TILE_DOWNLOAD_MAX_BYTES;
    result.status_code = esp_http_client_get_status_code(client);
    result.retry_after_sec = headers.retry_after_sec;
    result.content_type_valid = png_content_type(headers.content_type);
    if (result.status_code == 429) {
        download_step(&result, "rate_limited", ESP_ERR_TIMEOUT, NULL);
        ret = result.last_error;
        goto fetch_done;
    }
    if (result.status_code != 200) {
        download_step(&result, "http_status", ESP_FAIL, NULL);
        ret = result.last_error;
        goto fetch_done;
    }
    if (!result.content_type_valid) {
        download_step(&result, "content_type", ESP_ERR_INVALID_RESPONSE, NULL);
        ret = result.last_error;
        goto fetch_done;
    }
    if (content_length_known && content_length > (int64_t)download_limit) {
        download_step(&result, "content_length", ESP_ERR_INVALID_SIZE, NULL);
        ret = result.last_error;
        goto fetch_done;
    }

    int idle_reads = 0;
    while (!esp_http_client_is_complete_data_received(client)) {
        if (!continue_allowed(should_continue, continue_context)) {
            result.cancelled = true;
            download_step(&result, "cancelled", ESP_ERR_INVALID_STATE, NULL);
            ret = result.last_error;
            goto fetch_done;
        }
        const size_t remaining = download_limit - result.bytes;
        const size_t want = remaining < D1L_RP2040_FILE_CHUNK_MAX ?
                            remaining : D1L_RP2040_FILE_CHUNK_MAX;
        if (want == 0U) {
            download_step(&result, "too_large", ESP_ERR_INVALID_SIZE, NULL);
            ret = result.last_error;
            goto fetch_done;
        }
        const int read_len = esp_http_client_read(client, (char *)&buffer[result.bytes], want);
        if (read_len < 0) {
            download_step(&result, "http_read", ESP_FAIL, NULL);
            ret = result.last_error;
            goto fetch_done;
        }
        if ((size_t)read_len > want) {
            download_step(&result, "too_large", ESP_ERR_INVALID_SIZE, NULL);
            ret = result.last_error;
            goto fetch_done;
        }
        if (read_len == 0) {
            if (++idle_reads > 3) {
                break;
            }
            continue;
        }
        idle_reads = 0;
        ret = d1l_rp2040_bridge_file_write(result.tmp_path, (uint32_t)result.bytes,
                                           &buffer[result.bytes], (size_t)read_len,
                                           result.bytes == 0U, &file,
                                           D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        if (ret != ESP_OK || file.length != (uint32_t)read_len) {
            download_step(&result, "write_tmp", ret == ESP_OK ? ESP_FAIL : ret, &file);
            ret = result.last_error;
            goto fetch_done;
        }
        result.write_tmp = true;
        result.bytes += (size_t)read_len;
    }
    if (!esp_http_client_is_complete_data_received(client) || result.bytes == 0U ||
        (content_length_known && result.bytes != (size_t)content_length)) {
        download_step(&result, "http_incomplete", ESP_FAIL, NULL);
        ret = result.last_error;
        goto fetch_done;
    }
    result.png_valid = d1l_map_tile_png_valid(buffer, result.bytes);
    if (!result.png_valid) {
        download_step(&result, "png", ESP_ERR_INVALID_RESPONSE, NULL);
        ret = result.last_error;
        goto fetch_done;
    }

    ret = d1l_rp2040_bridge_file_rename(result.tmp_path, result.path, true, &file,
                                        D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        download_step(&result, "rename_final", ret, &file);
        goto fetch_done;
    }
    result.rename_replace = true;
    ret = write_attribution_metadata(&provider, &result);
    if (ret != ESP_OK) {
        /* A cache entry is only valid when its required attribution metadata
         * commits too.  Do not leave a newly-renamed tile paired with stale or
         * absent metadata for the next boot to accept. */
        d1l_rp2040_file_result_t delete_result = {0};
        (void)d1l_rp2040_bridge_file_delete(result.path, &delete_result,
                                            D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        (void)d1l_rp2040_bridge_file_delete(result.attribution_tmp_path, &delete_result,
                                            D1L_MAP_TILE_SD_FILE_TIMEOUT_MS);
        result.rename_replace = false;
        goto fetch_done;
    }
    download_step(&result, "ok", ESP_OK, &file);
    *out_len = result.bytes;

fetch_done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (ret != ESP_OK || result.last_error != ESP_OK) {
        cleanup_partial(&result);
    }
    *out_result = result;
    return result.last_error;
}

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
