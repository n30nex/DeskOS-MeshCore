#include "map_tile_provider.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "hal/rp2040_bridge.h"
#include "storage/map_tile_store.h"

#define D1L_MAP_PROVIDER_FILE_TIMEOUT_MS 10000U
#define D1L_MAP_PROVIDER_DEFAULT_AVERAGE_TILE_BYTES (64U * 1024U)

static portMUX_TYPE s_provider_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_map_tile_provider_t s_provider;
static bool s_provider_initialized;

static bool source_id_valid(const char *value)
{
    if (!value || value[0] == '\0') {
        return false;
    }
    size_t length = 0U;
    while (value[length] != '\0') {
        const unsigned char ch = (unsigned char)value[length];
        if (!((ch >= 'A' && ch <= 'Z') ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_')) {
            return false;
        }
        if (++length > D1L_MAP_PROVIDER_SOURCE_ID_MAX) {
            return false;
        }
    }
    return strcmp(value, D1L_MAP_TILE_SOURCE_ID) != 0;
}

static bool safe_text(const char *value, size_t capacity)
{
    if (!value || value[0] == '\0') {
        return false;
    }
    for (size_t i = 0U; i < capacity; ++i) {
        const unsigned char ch = (unsigned char)value[i];
        if (ch == '\0') {
            return true;
        }
        if (ch < 32U || ch > 126U || ch == '"' || ch == '\\') {
            return false;
        }
    }
    return false;
}

static size_t token_count(const char *text, const char *token)
{
    size_t count = 0U;
    if (!text || !token || token[0] == '\0') {
        return 0U;
    }
    const size_t token_length = strlen(token);
    const char *cursor = text;
    while ((cursor = strstr(cursor, token)) != NULL) {
        ++count;
        cursor += token_length;
    }
    return count;
}

static bool url_template_valid(const char *value)
{
    return safe_text(value, D1L_MAP_PROVIDER_URL_MAX + 1U) &&
           strncmp(value, "https://", 8U) == 0 &&
           token_count(value, "{z}") == 1U &&
           token_count(value, "{x}") == 1U &&
           token_count(value, "{y}") == 1U &&
           strchr(value, '#') == NULL;
}

static const char *json_value(const char *json, const char *key)
{
    if (!json || !key) {
        return NULL;
    }
    char needle[64];
    const int written = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (written <= 0 || (size_t)written >= sizeof(needle)) {
        return NULL;
    }
    const char *cursor = strstr(json, needle);
    if (!cursor) {
        return NULL;
    }
    cursor += (size_t)written;
    while (*cursor && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor++ != ':') {
        return NULL;
    }
    while (*cursor && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    return cursor;
}

static bool json_string(const char *json,
                        const char *key,
                        char *dest,
                        size_t dest_size)
{
    if (!dest || dest_size == 0U) {
        return false;
    }
    dest[0] = '\0';
    const char *cursor = json_value(json, key);
    if (!cursor || *cursor++ != '"') {
        return false;
    }
    size_t length = 0U;
    while (*cursor && *cursor != '"') {
        const unsigned char ch = (unsigned char)*cursor++;
        if (ch < 32U || ch > 126U || ch == '\\' ||
            length + 1U >= dest_size) {
            dest[0] = '\0';
            return false;
        }
        dest[length++] = (char)ch;
    }
    if (*cursor != '"' || length == 0U) {
        dest[0] = '\0';
        return false;
    }
    dest[length] = '\0';
    return true;
}

static bool json_bool(const char *json, const char *key, bool *out_value)
{
    if (!out_value) {
        return false;
    }
    const char *cursor = json_value(json, key);
    if (!cursor) {
        return false;
    }
    if (strncmp(cursor, "true", 4U) == 0 &&
        (cursor[4] == ',' || cursor[4] == '}' ||
         isspace((unsigned char)cursor[4]))) {
        *out_value = true;
        return true;
    }
    if (strncmp(cursor, "false", 5U) == 0 &&
        (cursor[5] == ',' || cursor[5] == '}' ||
         isspace((unsigned char)cursor[5]))) {
        *out_value = false;
        return true;
    }
    return false;
}

static bool json_u32(const char *json, const char *key, uint32_t *out_value)
{
    if (!out_value) {
        return false;
    }
    const char *cursor = json_value(json, key);
    if (!cursor || !isdigit((unsigned char)*cursor)) {
        return false;
    }
    char *end = NULL;
    const unsigned long value = strtoul(cursor, &end, 10);
    if (end == cursor || value > UINT32_MAX ||
        !(*end == ',' || *end == '}' || isspace((unsigned char)*end))) {
        return false;
    }
    *out_value = (uint32_t)value;
    return true;
}

void d1l_map_tile_provider_builtin(d1l_map_tile_provider_t *out_provider)
{
    if (!out_provider) {
        return;
    }
    memset(out_provider, 0, sizeof(*out_provider));
    snprintf(out_provider->source_id, sizeof(out_provider->source_id), "%s",
             D1L_MAP_TILE_SOURCE_ID);
    snprintf(out_provider->url_template, sizeof(out_provider->url_template), "%s",
             D1L_MAP_TILE_SOURCE_URL_TEMPLATE);
    snprintf(out_provider->attribution, sizeof(out_provider->attribution), "%s",
             D1L_MAP_TILE_ATTRIBUTION);
    snprintf(out_provider->license_url, sizeof(out_provider->license_url), "%s",
             D1L_MAP_TILE_LICENSE_URL);
    out_provider->network_fetch_allowed = true;
    out_provider->offline_storage_permitted = false;
    out_provider->background_prefetch_permitted = false;
    out_provider->max_zoom = D1L_MAP_TILE_ZOOM_MAX;
    out_provider->average_tile_bytes =
        D1L_MAP_PROVIDER_DEFAULT_AVERAGE_TILE_BYTES;
    out_provider->minimum_request_interval_ms =
        D1L_MAP_PROVIDER_REQUEST_INTERVAL_DEFAULT_MS;
}

static void ensure_initialized(void)
{
    portENTER_CRITICAL(&s_provider_lock);
    if (!s_provider_initialized) {
        d1l_map_tile_provider_builtin(&s_provider);
        s_provider_initialized = true;
    }
    portEXIT_CRITICAL(&s_provider_lock);
}

void d1l_map_tile_provider_snapshot(d1l_map_tile_provider_t *out_provider)
{
    if (!out_provider) {
        return;
    }
    ensure_initialized();
    portENTER_CRITICAL(&s_provider_lock);
    *out_provider = s_provider;
    portEXIT_CRITICAL(&s_provider_lock);
}

static esp_err_t read_provider_config(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_rp2040_file_result_t stat = {0};
    esp_err_t ret = d1l_rp2040_bridge_file_stat(
        D1L_MAP_PROVIDER_CONFIG_PATH, &stat,
        D1L_MAP_PROVIDER_FILE_TIMEOUT_MS);
    if (ret != ESP_OK || !stat.ok || !stat.exists || stat.is_directory) {
        return ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;
    }
    if (stat.size == 0U || stat.size >= buffer_size ||
        stat.size > D1L_MAP_PROVIDER_CONFIG_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t offset = 0U;
    while (offset < stat.size) {
        const size_t remaining = (size_t)stat.size - offset;
        const size_t requested =
            remaining < D1L_RP2040_FILE_CHUNK_MAX ?
                remaining : D1L_RP2040_FILE_CHUNK_MAX;
        d1l_rp2040_file_result_t read = {0};
        ret = d1l_rp2040_bridge_file_read(
            D1L_MAP_PROVIDER_CONFIG_PATH, (uint32_t)offset,
            (uint8_t *)&buffer[offset], requested, &read,
            D1L_MAP_PROVIDER_FILE_TIMEOUT_MS);
        if (ret != ESP_OK || !read.ok || read.offset != offset ||
            read.length == 0U || read.length > requested) {
            return ret == ESP_OK ? ESP_FAIL : ret;
        }
        offset += read.length;
    }
    buffer[offset] = '\0';
    return ESP_OK;
}

static esp_err_t parse_provider_config(
    const char *json,
    d1l_map_tile_provider_t *out_provider)
{
    if (!json || !out_provider) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_map_tile_provider_t provider = {0};
    uint32_t schema = 0U;
    uint32_t max_zoom = 0U;
    uint32_t average_tile_bytes = 0U;
    uint32_t minimum_request_interval_ms =
        D1L_MAP_PROVIDER_REQUEST_INTERVAL_DEFAULT_MS;
    bool offline_allowed = false;
    bool background_allowed = false;
    if (!json_u32(json, "schema", &schema) || schema != 1U ||
        !json_string(json, "source_id", provider.source_id,
                     sizeof(provider.source_id)) ||
        !json_string(json, "attribution", provider.attribution,
                     sizeof(provider.attribution)) ||
        !json_string(json, "license_url", provider.license_url,
                     sizeof(provider.license_url)) ||
        !json_bool(json, "offline_storage_permitted", &offline_allowed) ||
        !json_bool(json, "background_prefetch_permitted",
                   &background_allowed) ||
        !json_u32(json, "max_zoom", &max_zoom) ||
        !json_u32(json, "average_tile_bytes", &average_tile_bytes)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!source_id_valid(provider.source_id) ||
        !safe_text(provider.attribution, sizeof(provider.attribution)) ||
        !safe_text(provider.license_url, sizeof(provider.license_url)) ||
        strncmp(provider.license_url, "https://", 8U) != 0 ||
        !offline_allowed || max_zoom < 14U ||
        max_zoom > D1L_MAP_TILE_ZOOM_MAX ||
        average_tile_bytes < 4096U ||
        average_tile_bytes > D1L_MAP_TILE_DOWNLOAD_MAX_BYTES) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const char *request_interval =
        json_value(json, "minimum_request_interval_ms");
    if (request_interval && strncmp(request_interval, "null", 4U) != 0 &&
        (!json_u32(json, "minimum_request_interval_ms",
                   &minimum_request_interval_ms) ||
         minimum_request_interval_ms <
             D1L_MAP_PROVIDER_REQUEST_INTERVAL_MIN_MS ||
         minimum_request_interval_ms >
             D1L_MAP_PROVIDER_REQUEST_INTERVAL_MAX_MS)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *url = json_value(json, "network_url_template");
    if (url && strncmp(url, "null", 4U) != 0) {
        if (!json_string(json, "network_url_template",
                         provider.url_template,
                         sizeof(provider.url_template)) ||
            !url_template_valid(provider.url_template)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        provider.network_fetch_allowed = true;
    }
    if (background_allowed && !provider.network_fetch_allowed) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    provider.configured = true;
    provider.offline_storage_permitted = true;
    provider.background_prefetch_permitted = background_allowed;
    provider.max_zoom = (uint8_t)max_zoom;
    provider.average_tile_bytes = average_tile_bytes;
    provider.minimum_request_interval_ms = minimum_request_interval_ms;
    *out_provider = provider;
    return ESP_OK;
}

esp_err_t d1l_map_tile_provider_refresh(
    const d1l_storage_status_t *storage)
{
    ensure_initialized();
    if (!storage || !d1l_map_tile_store_sd_ready(storage)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    char json[D1L_MAP_PROVIDER_CONFIG_MAX_BYTES + 1U];
    const esp_err_t read_ret = read_provider_config(json, sizeof(json));
    d1l_map_tile_provider_t provider = {0};
    esp_err_t ret = read_ret;
    if (ret == ESP_OK) {
        ret = parse_provider_config(json, &provider);
    }
    if (ret == ESP_ERR_NOT_FOUND) {
        d1l_map_tile_provider_builtin(&provider);
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_provider_lock);
        s_provider = provider;
        s_provider_initialized = true;
        portEXIT_CRITICAL(&s_provider_lock);
    }
    memset(json, 0, sizeof(json));
    return ret;
}

bool d1l_map_tile_provider_path(
    const d1l_map_tile_provider_t *provider,
    uint8_t z,
    uint32_t x,
    uint32_t y,
    char *dest,
    size_t dest_size)
{
    if (!provider || !dest || dest_size == 0U ||
        z > provider->max_zoom ||
        !d1l_map_tile_store_coord_valid(z, x, y)) {
        return false;
    }
    const char *directory = provider->configured ?
        provider->source_id : "openstreetmap";
    const int written = snprintf(
        dest, dest_size, "map/tiles/%s/z%u/x%lu/y%lu.png",
        directory, (unsigned)z, (unsigned long)x, (unsigned long)y);
    return written > 0 && (size_t)written < dest_size;
}

bool d1l_map_tile_provider_attribution_path(
    const d1l_map_tile_provider_t *provider,
    bool temporary,
    char *dest,
    size_t dest_size)
{
    if (!provider || !dest || dest_size == 0U) {
        return false;
    }
    const char *directory = provider->configured ?
        provider->source_id : "openstreetmap";
    const int written = snprintf(
        dest, dest_size, "map/tiles/%s/attribution.%s",
        directory, temporary ? "tmp" : "json");
    return written > 0 && (size_t)written < dest_size;
}
