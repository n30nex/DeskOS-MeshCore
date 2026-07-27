#include "map_tile_provider.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "hal/rp2040_bridge.h"
#include "mbedtls/sha256.h"
#include "mesh/route_store_worker.h"
#include "storage/map_tile_store.h"

#define D1L_MAP_PROVIDER_FILE_TIMEOUT_MS 10000U
#define D1L_MAP_PROVIDER_STAGE_PATH_FORMAT \
    "map/offline-provider.stage-rc1-%03u.json"
#define D1L_MAP_PROVIDER_BACKUP_PATH_FORMAT \
    "map/offline-provider.invalid-rc1-%03u.json"
#define D1L_MAP_PROVIDER_FIXED_BACKUP_PATH \
    "map/offline-provider.invalid-rc1.json"
#define D1L_MAP_PROVIDER_PATH_SEQUENCE_MAX 999U
#define D1L_MAP_PROVIDER_DEFAULT_AVERAGE_TILE_BYTES (64U * 1024U)
#define D1L_MAP_PROVIDER_REPAIR_MANAGER_QUIESCE_TIMEOUT_MS 15000U

static const char s_default_provider_manifest[] =
    "{\"schema\":1,"
    "\"source_id\":\"nrcan-cbmt\","
    "\"attribution\":\"Natural Resources Canada; Open Government Licence - Canada\","
    "\"license_url\":\"https://open.canada.ca/en/open-government-licence-canada\","
    "\"offline_storage_permitted\":true,"
    "\"background_prefetch_permitted\":true,"
    "\"network_url_template\":\"https://maps.geogratis.gc.ca/wms/CBMT?"
    "mode=tile&tilemode=gmap&layers=National%20Sub_national%20Regional%20Sub_regional"
    "&tile={x}+{y}+{z}\","
    "\"tile_template\":\"z{z}/x{x}/y{y}.png\","
    "\"max_zoom\":15,"
    "\"average_tile_bytes\":65536,"
    "\"minimum_request_interval_ms\":1000}\n";

_Static_assert(
    sizeof(s_default_provider_manifest) - 1U ==
        D1L_MAP_PROVIDER_DEFAULT_MANIFEST_BYTES,
    "default map provider manifest byte count changed");
_Static_assert(
    D1L_MAP_PROVIDER_DEFAULT_MANIFEST_BYTES <=
        D1L_MAP_PROVIDER_CONFIG_MAX_BYTES,
    "default map provider manifest exceeds configured maximum");

static const uint8_t s_default_provider_manifest_sha256[32] = {
    0xe7U, 0xdaU, 0x7aU, 0x25U, 0x69U, 0x54U, 0x61U, 0x7fU,
    0x80U, 0x8bU, 0x16U, 0xf1U, 0x30U, 0x6bU, 0x86U, 0x84U,
    0xc9U, 0x10U, 0x25U, 0xa6U, 0x7cU, 0xd4U, 0x98U, 0x41U,
    0xa2U, 0x6eU, 0xfbU, 0xc2U, 0xcaU, 0x25U, 0x39U, 0x84U,
};

static portMUX_TYPE s_provider_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_map_tile_provider_t s_provider;
static bool s_provider_initialized;
static bool s_provider_io_busy;

static bool provider_io_claim(void)
{
    bool claimed = false;
    portENTER_CRITICAL(&s_provider_lock);
    if (!s_provider_io_busy) {
        s_provider_io_busy = true;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_provider_lock);
    return claimed;
}

static void provider_io_release(void)
{
    portENTER_CRITICAL(&s_provider_lock);
    s_provider_io_busy = false;
    portEXIT_CRITICAL(&s_provider_lock);
}

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

static bool ascii_equal_case_insensitive(const char *left,
                                         size_t left_length,
                                         const char *right)
{
    if (!left || !right || strlen(right) != left_length) {
        return false;
    }
    for (size_t i = 0U; i < left_length; ++i) {
        if (tolower((unsigned char)left[i]) !=
            tolower((unsigned char)right[i])) {
            return false;
        }
    }
    return true;
}

static bool osm_standard_url(const char *value)
{
    static const char host[] = "tile.openstreetmap.org";
    if (!value || strncmp(value, "https://", 8U) != 0) {
        return false;
    }
    const size_t host_length = sizeof(host) - 1U;
    const size_t value_length = strlen(value);
    for (size_t i = 8U; i + host_length <= value_length; ++i) {
        if (ascii_equal_case_insensitive(
                &value[i], host_length, host)) {
            return true;
        }
    }
    return false;
}

bool d1l_map_tile_provider_uses_osm_standard(
    const d1l_map_tile_provider_t *provider)
{
    return provider &&
           (strcmp(provider->source_id, D1L_MAP_TILE_SOURCE_ID) == 0 ||
            osm_standard_url(provider->url_template));
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

static esp_err_t read_provider_path(const char *path,
                                    char *buffer,
                                    size_t buffer_size,
                                    size_t *out_size)
{
    if (!path || !buffer || buffer_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_size) {
        *out_size = 0U;
    }
    d1l_rp2040_file_result_t stat = {0};
    esp_err_t ret = d1l_rp2040_bridge_file_stat(
        path, &stat,
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
            path, (uint32_t)offset,
            (uint8_t *)&buffer[offset], requested, &read,
            D1L_MAP_PROVIDER_FILE_TIMEOUT_MS);
        if (ret != ESP_OK || !read.ok || read.offset != offset ||
            read.length == 0U || read.length > requested) {
            return ret == ESP_OK ? ESP_FAIL : ret;
        }
        offset += read.length;
    }
    buffer[offset] = '\0';
    if (out_size) {
        *out_size = offset;
    }
    return ESP_OK;
}

static esp_err_t read_provider_config(char *buffer, size_t buffer_size)
{
    return read_provider_path(
        D1L_MAP_PROVIDER_CONFIG_PATH, buffer, buffer_size, NULL);
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
    /*
     * The built-in OSM Standard endpoint is current-view-only. Never accept a
     * removable-media manifest that attempts to grant offline or bulk rights
     * for that endpoint.
     */
    if (d1l_map_tile_provider_uses_osm_standard(&provider)) {
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

typedef struct {
    bool bytes_exact;
    bool hash_verified;
    bool parsed;
    size_t bytes;
    d1l_map_tile_provider_t provider;
} d1l_default_provider_validation_t;

static esp_err_t provider_path_exists(const char *path, bool *out_exists)
{
    if (!path || !out_exists) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_exists = false;
    d1l_rp2040_file_result_t file = {0};
    const esp_err_t ret = d1l_rp2040_bridge_file_stat(
        path, &file, D1L_MAP_PROVIDER_FILE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    *out_exists = file.exists;
    return ESP_OK;
}

static esp_err_t select_absent_enumerated_path(
    const char *path_format,
    char *out_path,
    size_t out_path_size)
{
    if (!path_format || !out_path || out_path_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    out_path[0] = '\0';
    for (unsigned sequence = 1U;
         sequence <= D1L_MAP_PROVIDER_PATH_SEQUENCE_MAX;
         ++sequence) {
        const int written = snprintf(
            out_path, out_path_size, path_format, sequence);
        if (written <= 0 || (size_t)written >= out_path_size) {
            out_path[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
        bool exists = false;
        const esp_err_t ret = provider_path_exists(out_path, &exists);
        if (ret != ESP_OK) {
            out_path[0] = '\0';
            return ret;
        }
        if (!exists) {
            return ESP_OK;
        }
    }
    out_path[0] = '\0';
    return ESP_ERR_INVALID_STATE;
}

typedef struct {
    bool attempted;
    bool performed;
    bool completion_uncertain;
    bool create_new;
} d1l_provider_create_new_result_t;

static bool create_failure_proves_no_file_mutation(
    const d1l_rp2040_file_result_t *file)
{
    if (!file || !file->bridge_ready) {
        return true;
    }
    if (!file->protocol_supported || file->ok) {
        return false;
    }
    return strcmp(file->err, "exists") == 0 ||
           strcmp(file->err, "no_card") == 0 ||
           strcmp(file->err, "not_ready") == 0 ||
           strcmp(file->err, "bad_path") == 0 ||
           strcmp(file->err, "bad_request") == 0 ||
           strcmp(file->err, "bad_value") == 0 ||
           strcmp(file->err, "decode_failed") == 0 ||
           strcmp(file->err, "crc_mismatch") == 0 ||
           strcmp(file->err, "range") == 0 ||
           strcmp(file->err, "too_large") == 0 ||
           strcmp(file->err, "line_too_long") == 0 ||
           strcmp(file->err, "unsupported_op") == 0;
}

static esp_err_t write_provider_create_new(
    const char *path,
    const char *payload,
    size_t payload_size,
    d1l_provider_create_new_result_t *out_result)
{
    if (!path || path[0] == '\0' || !payload || payload_size == 0U ||
        payload_size > D1L_MAP_PROVIDER_CONFIG_MAX_BYTES || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_result, 0, sizeof(*out_result));
    d1l_rp2040_file_result_t file = {0};
    size_t offset = 0U;
    while (offset < payload_size) {
        const size_t remaining = payload_size - offset;
        const size_t chunk =
            remaining < D1L_RP2040_FILE_CHUNK_MAX ?
                remaining : D1L_RP2040_FILE_CHUNK_MAX;
        esp_err_t ret = ESP_OK;
        if (offset == 0U) {
            out_result->attempted = true;
            ret = d1l_rp2040_bridge_file_create(
                path, (const uint8_t *)&payload[offset], chunk,
                &file, D1L_MAP_PROVIDER_FILE_TIMEOUT_MS);
            if (ret == ESP_OK) {
                out_result->performed = true;
                out_result->create_new = true;
            } else if (!create_failure_proves_no_file_mutation(&file)) {
                out_result->completion_uncertain = true;
            }
        } else {
            ret = d1l_rp2040_bridge_file_write(
                path, (uint32_t)offset,
                (const uint8_t *)&payload[offset], chunk,
                false, &file, D1L_MAP_PROVIDER_FILE_TIMEOUT_MS);
            if (ret != ESP_OK) {
                out_result->completion_uncertain = true;
            }
        }
        if (ret != ESP_OK || file.length != (uint32_t)chunk) {
            if (ret == ESP_OK) {
                out_result->completion_uncertain = true;
            }
            return ret == ESP_OK ? ESP_FAIL : ret;
        }
        offset += chunk;
    }
    return ESP_OK;
}

static esp_err_t write_default_provider_stage(
    const char *stage_path,
    d1l_provider_create_new_result_t *out_result)
{
    return write_provider_create_new(
        stage_path, s_default_provider_manifest,
        sizeof(s_default_provider_manifest) - 1U, out_result);
}

static bool provider_sha256(
    const char *buffer,
    size_t size,
    uint8_t digest[32])
{
    if (!buffer || !digest) {
        return false;
    }
    memset(digest, 0, 32U);
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    int hash_ret = mbedtls_sha256_starts(&context, 0);
    if (hash_ret == 0) {
        hash_ret = mbedtls_sha256_update(
            &context, (const uint8_t *)buffer, size);
    }
    if (hash_ret == 0) {
        hash_ret = mbedtls_sha256_finish(&context, digest);
    }
    mbedtls_sha256_free(&context);
    return hash_ret == 0;
}

static bool provider_sha256_hex(
    const char *buffer,
    size_t size,
    char out_hex[D1L_MAP_PROVIDER_SHA256_HEX_LENGTH + 1U])
{
    if (!out_hex) {
        return false;
    }
    out_hex[0] = '\0';
    uint8_t digest[32] = {0};
    if (!provider_sha256(buffer, size, digest)) {
        return false;
    }
    for (size_t i = 0U; i < sizeof(digest); ++i) {
        (void)snprintf(&out_hex[i * 2U], 3U, "%02x", digest[i]);
    }
    out_hex[D1L_MAP_PROVIDER_SHA256_HEX_LENGTH] = '\0';
    memset(digest, 0, sizeof(digest));
    return true;
}

static bool default_provider_hash_matches(const char *buffer, size_t size)
{
    if (!buffer || size != D1L_MAP_PROVIDER_DEFAULT_MANIFEST_BYTES) {
        return false;
    }
    uint8_t digest[sizeof(s_default_provider_manifest_sha256)] = {0};
    const bool matches =
        provider_sha256(buffer, size, digest) &&
        memcmp(
            digest, s_default_provider_manifest_sha256,
            sizeof(s_default_provider_manifest_sha256)) == 0;
    memset(digest, 0, sizeof(digest));
    return matches;
}

static esp_err_t validate_default_provider_buffer(
    const char *buffer,
    size_t size,
    d1l_default_provider_validation_t *out_validation)
{
    if (!buffer || !out_validation) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_validation, 0, sizeof(*out_validation));
    out_validation->bytes = size;
    if (size != D1L_MAP_PROVIDER_DEFAULT_MANIFEST_BYTES ||
        memcmp(buffer, s_default_provider_manifest, size) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out_validation->bytes_exact = true;
    if (!default_provider_hash_matches(buffer, size)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out_validation->hash_verified = true;
    const esp_err_t ret = parse_provider_config(
        buffer, &out_validation->provider);
    if (ret != ESP_OK ||
        strcmp(out_validation->provider.source_id, "nrcan-cbmt") != 0) {
        return ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : ret;
    }
    out_validation->parsed = true;
    return ESP_OK;
}

static esp_err_t validate_default_provider_path(
    const char *path,
    char *buffer,
    size_t buffer_size,
    d1l_default_provider_validation_t *out_validation)
{
    if (!path || !buffer || !out_validation) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t size = 0U;
    const esp_err_t ret = read_provider_path(
        path, buffer, buffer_size, &size);
    if (ret != ESP_OK) {
        memset(out_validation, 0, sizeof(*out_validation));
        return ret;
    }
    return validate_default_provider_buffer(buffer, size, out_validation);
}

static esp_err_t inspect_provider_path(
    const char *path,
    d1l_map_provider_path_inspection_t *out_inspection,
    char *buffer,
    size_t buffer_size)
{
    if (!path || !out_inspection || !buffer || buffer_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_inspection, 0, sizeof(*out_inspection));
    snprintf(
        out_inspection->path, sizeof(out_inspection->path), "%s", path);

    d1l_rp2040_file_result_t stat = {0};
    esp_err_t ret = d1l_rp2040_bridge_file_stat(
        path, &stat, D1L_MAP_PROVIDER_FILE_TIMEOUT_MS);
    out_inspection->io_result = ret;
    if (ret != ESP_OK) {
        return ret;
    }
    out_inspection->exists = stat.exists;
    out_inspection->is_directory = stat.is_directory;
    out_inspection->attributes_valid = stat.attributes_valid;
    out_inspection->attributes = stat.attributes;
    out_inspection->bytes = stat.size;
    if (!stat.exists) {
        return ESP_OK;
    }
    if (stat.is_directory) {
        out_inspection->io_result = ESP_ERR_INVALID_STATE;
        return out_inspection->io_result;
    }

    size_t bytes = 0U;
    ret = read_provider_path(path, buffer, buffer_size, &bytes);
    out_inspection->io_result = ret;
    if (ret != ESP_OK) {
        return ret;
    }
    out_inspection->read_ok = true;
    out_inspection->bytes = bytes;
    out_inspection->hash_calculated = provider_sha256_hex(
        buffer, bytes, out_inspection->sha256);
    if (!out_inspection->hash_calculated) {
        out_inspection->io_result = ESP_FAIL;
        return out_inspection->io_result;
    }

    d1l_map_tile_provider_t provider = {0};
    const esp_err_t parse_ret = parse_provider_config(buffer, &provider);
    if (parse_ret == ESP_OK) {
        out_inspection->parse_valid = true;
        snprintf(
            out_inspection->source_id,
            sizeof(out_inspection->source_id), "%s", provider.source_id);
    } else if (parse_ret == ESP_ERR_INVALID_RESPONSE) {
        out_inspection->parse_invalid = true;
    } else {
        out_inspection->io_result = parse_ret;
        return parse_ret;
    }

    d1l_default_provider_validation_t validation = {0};
    out_inspection->builtin_exact =
        validate_default_provider_buffer(
            buffer, bytes, &validation) == ESP_OK;
    out_inspection->io_result = ESP_OK;
    return ESP_OK;
}

static esp_err_t seed_default_provider_config(void)
{
    char stage_path[D1L_MAP_PROVIDER_BACKUP_PATH_MAX + 1U];
    char verify[D1L_MAP_PROVIDER_CONFIG_MAX_BYTES + 1U];
    d1l_default_provider_validation_t validation = {0};
    d1l_provider_create_new_result_t stage_write = {0};
    esp_err_t ret = select_absent_enumerated_path(
        D1L_MAP_PROVIDER_STAGE_PATH_FORMAT, stage_path, sizeof(stage_path));
    if (ret == ESP_OK) {
        ret = write_default_provider_stage(stage_path, &stage_write);
    }
    if (ret == ESP_OK) {
        ret = validate_default_provider_path(
            stage_path, verify, sizeof(verify), &validation);
    }
    if (ret == ESP_OK) {
        /*
         * The stage was absent when selected and is exact, hash-bound, and
         * parsed before this forward commit. Never replace a provider that
         * appears concurrently, and leave a failed stage as evidence rather
         * than deleting or rolling it back.
         */
        d1l_rp2040_file_result_t file = {0};
        ret = d1l_rp2040_bridge_file_rename(
            stage_path, D1L_MAP_PROVIDER_CONFIG_PATH, false, &file,
            D1L_MAP_PROVIDER_FILE_TIMEOUT_MS);
    }
    memset(verify, 0, sizeof(verify));
    return ret;
}

static esp_err_t read_invalid_provider_backup(
    const char *path,
    char *buffer,
    size_t buffer_size,
    size_t *out_size)
{
    const esp_err_t read_ret = read_provider_path(
        path, buffer, buffer_size, out_size);
    if (read_ret != ESP_OK) {
        return read_ret;
    }
    d1l_map_tile_provider_t provider = {0};
    const esp_err_t parse_ret = parse_provider_config(buffer, &provider);
    if (parse_ret == ESP_ERR_INVALID_RESPONSE) {
        return ESP_OK;
    }
    return parse_ret == ESP_OK ? ESP_ERR_INVALID_STATE : parse_ret;
}

static esp_err_t find_preserved_invalid_backup(
    char *out_path,
    size_t out_path_size,
    char *buffer,
    size_t buffer_size,
    size_t *out_size)
{
    if (!out_path || out_path_size == 0U || !buffer || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }
    out_path[0] = '\0';
    *out_size = 0U;

    bool exists = false;
    esp_err_t ret = provider_path_exists(
        D1L_MAP_PROVIDER_FIXED_BACKUP_PATH, &exists);
    if (ret != ESP_OK) {
        return ret;
    }
    esp_err_t first_invalid_ret = ESP_ERR_NOT_FOUND;
    if (exists) {
        ret = read_invalid_provider_backup(
            D1L_MAP_PROVIDER_FIXED_BACKUP_PATH, buffer, buffer_size,
            out_size);
        if (ret == ESP_OK) {
            snprintf(
                out_path, out_path_size, "%s",
                D1L_MAP_PROVIDER_FIXED_BACKUP_PATH);
            return ESP_OK;
        }
        first_invalid_ret = ret;
    }

    for (unsigned sequence = 1U;
         sequence <= D1L_MAP_PROVIDER_PATH_SEQUENCE_MAX;
         ++sequence) {
        char candidate[D1L_MAP_PROVIDER_BACKUP_PATH_MAX + 1U];
        const int written = snprintf(
            candidate, sizeof(candidate),
            D1L_MAP_PROVIDER_BACKUP_PATH_FORMAT, sequence);
        if (written <= 0 || (size_t)written >= sizeof(candidate)) {
            return ESP_ERR_INVALID_SIZE;
        }
        ret = provider_path_exists(candidate, &exists);
        if (ret != ESP_OK) {
            return ret;
        }
        if (!exists) {
            break;
        }
        ret = read_invalid_provider_backup(
            candidate, buffer, buffer_size, out_size);
        if (ret == ESP_OK) {
            snprintf(out_path, out_path_size, "%s", candidate);
            return ESP_OK;
        }
        if (first_invalid_ret == ESP_ERR_NOT_FOUND) {
            first_invalid_ret = ret;
        }
    }
    return first_invalid_ret;
}

esp_err_t d1l_map_tile_provider_refresh(
    const d1l_storage_status_t *storage)
{
    ensure_initialized();
    if (!storage || !d1l_map_tile_store_sd_ready(storage)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!provider_io_claim()) {
        return ESP_ERR_INVALID_STATE;
    }
    char json[D1L_MAP_PROVIDER_CONFIG_MAX_BYTES + 1U];
    esp_err_t read_ret = read_provider_config(json, sizeof(json));
    if (read_ret == ESP_ERR_NOT_FOUND) {
        char preserved_path[D1L_MAP_PROVIDER_BACKUP_PATH_MAX + 1U];
        size_t preserved_size = 0U;
        const esp_err_t recovery_ret = find_preserved_invalid_backup(
            preserved_path, sizeof(preserved_path),
            json, sizeof(json), &preserved_size);
        if (recovery_ret == ESP_OK) {
            /*
             * A forward repair already owns the missing-canonical state.
             * Only the explicit recovery command may verify that backup and
             * install a new canonical provider.
             */
            read_ret = ESP_ERR_INVALID_STATE;
        } else if (recovery_ret == ESP_ERR_NOT_FOUND) {
            const esp_err_t seed_ret = seed_default_provider_config();
            read_ret = seed_ret == ESP_OK ?
                read_provider_config(json, sizeof(json)) : seed_ret;
        } else {
            read_ret = recovery_ret;
        }
    }
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
    provider_io_release();
    return ret;
}

esp_err_t d1l_map_tile_provider_inspect_recovery(
    const d1l_storage_status_t *storage,
    d1l_map_provider_recovery_inspection_t *out_inspection)
{
    if (!out_inspection) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_inspection, 0, sizeof(*out_inspection));
    snprintf(
        out_inspection->canonical.path,
        sizeof(out_inspection->canonical.path), "%s",
        D1L_MAP_PROVIDER_CONFIG_PATH);
    snprintf(
        out_inspection->stage_001.path,
        sizeof(out_inspection->stage_001.path), "%s",
        D1L_MAP_PROVIDER_RECOVERY_STAGE_001_PATH);
    snprintf(
        out_inspection->backup_001.path,
        sizeof(out_inspection->backup_001.path), "%s",
        D1L_MAP_PROVIDER_RECOVERY_BACKUP_001_PATH);
    if (!storage || !d1l_map_tile_store_sd_ready(storage)) {
        out_inspection->canonical.io_result = ESP_ERR_NOT_SUPPORTED;
        out_inspection->stage_001.io_result = ESP_ERR_NOT_SUPPORTED;
        out_inspection->backup_001.io_result = ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!provider_io_claim()) {
        out_inspection->provider_lock_busy = true;
        out_inspection->canonical.io_result = ESP_ERR_INVALID_STATE;
        out_inspection->stage_001.io_result = ESP_ERR_INVALID_STATE;
        out_inspection->backup_001.io_result = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }

    char buffer[D1L_MAP_PROVIDER_CONFIG_MAX_BYTES + 1U];
    esp_err_t first_error = inspect_provider_path(
        D1L_MAP_PROVIDER_CONFIG_PATH, &out_inspection->canonical,
        buffer, sizeof(buffer));
    const esp_err_t stage_ret = inspect_provider_path(
        D1L_MAP_PROVIDER_RECOVERY_STAGE_001_PATH,
        &out_inspection->stage_001, buffer, sizeof(buffer));
    if (first_error == ESP_OK && stage_ret != ESP_OK) {
        first_error = stage_ret;
    }
    const esp_err_t backup_ret = inspect_provider_path(
        D1L_MAP_PROVIDER_RECOVERY_BACKUP_001_PATH,
        &out_inspection->backup_001, buffer, sizeof(buffer));
    if (first_error == ESP_OK && backup_ret != ESP_OK) {
        first_error = backup_ret;
    }

    out_inspection->complete = first_error == ESP_OK;
    out_inspection->canonical_backup_bytes_equal =
        out_inspection->canonical.read_ok &&
        out_inspection->backup_001.read_ok &&
        out_inspection->canonical.bytes == out_inspection->backup_001.bytes &&
        strcmp(
            out_inspection->canonical.sha256,
            out_inspection->backup_001.sha256) == 0;
    memset(buffer, 0, sizeof(buffer));
    provider_io_release();
    return first_error;
}

static void provider_repair_set_stage(
    d1l_map_provider_repair_result_t *result,
    const char *stage)
{
    if (!result) {
        return;
    }
    snprintf(
        result->recovery_stage, sizeof(result->recovery_stage), "%s",
        stage ? stage : "unknown");
}

static esp_err_t provider_repair_quiesce_storage(
    d1l_map_provider_repair_result_t *result,
    bool *manager_quiesced,
    bool *retained_worker_quiesced)
{
    if (!result || !manager_quiesced || !retained_worker_quiesced) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!*manager_quiesced) {
        result->storage_manager_quiesce_attempted = true;
        const esp_err_t manager_ret = d1l_storage_manager_quiesce_begin(
            D1L_MAP_PROVIDER_REPAIR_MANAGER_QUIESCE_TIMEOUT_MS);
        if (manager_ret != ESP_OK) {
            return manager_ret;
        }
        *manager_quiesced = true;
        result->storage_manager_paused = true;
        result->storage_manager_quiesced = true;
    }
    if (*retained_worker_quiesced) {
        return ESP_OK;
    }

    /*
     * Match the storage manager's established lock order. The common retained
     * worker owns Public/DM/packet/route/node/contact/checkpoint SD traffic;
     * holding only the manager sequence still permits that producer to begin a
     * bridge file exchange between recovery reads and forward mutations.
     */
    result->retained_worker_quiesce_attempted = true;
    const esp_err_t retained_ret = d1l_route_store_worker_quiesce_begin(
        D1L_MAP_PROVIDER_REPAIR_MANAGER_QUIESCE_TIMEOUT_MS);
    if (retained_ret != ESP_OK) {
        return retained_ret;
    }
    *retained_worker_quiesced = true;
    result->retained_worker_quiesced = true;
    return ESP_OK;
}

static esp_err_t verify_invalid_provider_copy(
    const char *path,
    const char *expected,
    size_t expected_size,
    char *verify,
    size_t verify_size,
    d1l_map_provider_repair_result_t *result)
{
    if (!path || !expected || expected_size == 0U || !verify ||
        !result) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t copied_size = 0U;
    esp_err_t ret = read_provider_path(
        path, verify, verify_size, &copied_size);
    if (ret != ESP_OK) {
        return ret;
    }
    result->backup_bytes_verified =
        copied_size == expected_size &&
        memcmp(verify, expected, expected_size) == 0;

    uint8_t expected_hash[32] = {0};
    uint8_t copied_hash[32] = {0};
    result->backup_hash_verified =
        provider_sha256(expected, expected_size, expected_hash) &&
        provider_sha256(verify, copied_size, copied_hash) &&
        memcmp(expected_hash, copied_hash, sizeof(expected_hash)) == 0;
    memset(expected_hash, 0, sizeof(expected_hash));
    memset(copied_hash, 0, sizeof(copied_hash));
    if (!result->backup_bytes_verified ||
        !result->backup_hash_verified) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    d1l_map_tile_provider_t invalid_provider = {0};
    ret = parse_provider_config(verify, &invalid_provider);
    if (ret != ESP_ERR_INVALID_RESPONSE) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    result->backup_preserved = true;
    return ESP_OK;
}

esp_err_t d1l_map_tile_provider_repair_invalid_default(
    const d1l_storage_status_t *storage,
    d1l_map_provider_repair_result_t *out_result)
{
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_result, 0, sizeof(*out_result));
    snprintf(
        out_result->action, sizeof(out_result->action), "%s",
        "failed_closed");
    provider_repair_set_stage(out_result, "preflight");
    out_result->fixed_backup_untouched = true;
    ensure_initialized();
    if (!storage || !d1l_map_tile_store_sd_ready(storage)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!provider_io_claim()) {
        out_result->provider_lock_busy = true;
        snprintf(
            out_result->action, sizeof(out_result->action), "%s",
            "provider_lock_busy");
        return ESP_ERR_INVALID_STATE;
    }

    char before[D1L_MAP_PROVIDER_CONFIG_MAX_BYTES + 1U];
    char verify[D1L_MAP_PROVIDER_CONFIG_MAX_BYTES + 1U];
    size_t before_size = 0U;
    size_t verify_size = 0U;
    d1l_map_tile_provider_t provider = {0};
    d1l_default_provider_validation_t validation = {0};
    d1l_rp2040_file_result_t file = {0};
    bool fixed_backup_exists = false;
    bool repair_invalid = false;
    bool stage_001_exists = false;
    bool manager_quiesced = false;
    bool retained_worker_quiesced = false;
    bool canonical_exists = false;
    bool stage_exists = false;
    esp_err_t ret = ESP_OK;

    /*
     * A provider file is larger than one bridge chunk, so its stat/read
     * snapshot is not one RP2040 exchange. Acquire the established manager ->
     * retained-producer ownership order before the first canonical read and
     * keep it through every stage/backup read and forward mutation. Otherwise
     * a preflight buffer assembled while another producer uses the bridge can
     * differ from the stable reread even though both individual reads report
     * ESP_OK and the same byte count.
     */
    provider_repair_set_stage(out_result, "storage_quiesce");
    ret = provider_repair_quiesce_storage(
        out_result, &manager_quiesced, &retained_worker_quiesced);
    if (ret != ESP_OK) {
        goto done;
    }
    provider_repair_set_stage(out_result, "preflight");

    ret = provider_path_exists(
        D1L_MAP_PROVIDER_FIXED_BACKUP_PATH, &fixed_backup_exists);
    if (ret != ESP_OK) {
        goto done;
    }
    out_result->fixed_backup_present = fixed_backup_exists;

    ret = read_provider_path(
        D1L_MAP_PROVIDER_CONFIG_PATH, before, sizeof(before), &before_size);
    if (ret == ESP_ERR_NOT_FOUND) {
        out_result->canonical_missing_before = true;
        ret = find_preserved_invalid_backup(
            out_result->preserved_backup_path,
            sizeof(out_result->preserved_backup_path),
            before, sizeof(before), &before_size);
        if (ret != ESP_OK) {
            goto done;
        }
        out_result->recovery_resumed = true;
        out_result->backup_preexisting = true;
        out_result->backup_preserved = true;
        snprintf(
            out_result->backup_path, sizeof(out_result->backup_path), "%s",
            out_result->preserved_backup_path);
        snprintf(
            out_result->action, sizeof(out_result->action), "%s",
            "resuming_missing_canonical");
    } else if (ret != ESP_OK) {
        goto done;
    } else {
        out_result->canonical_initial_hash_calculated =
            provider_sha256_hex(
                before, before_size, out_result->canonical_initial_sha256);
        if (!out_result->canonical_initial_hash_calculated) {
            ret = ESP_FAIL;
            goto done;
        }
        ret = parse_provider_config(before, &provider);
        if (ret == ESP_OK) {
            out_result->before_valid = true;
            out_result->before_bytes = before_size;
            out_result->final_valid = true;
            out_result->final_bytes = before_size;
            if (validate_default_provider_buffer(
                    before, before_size, &validation) == ESP_OK) {
                out_result->final_builtin_exact = true;
            }
            snprintf(
                out_result->source_id, sizeof(out_result->source_id), "%s",
                provider.source_id);
            snprintf(
                out_result->action, sizeof(out_result->action), "%s",
                "preserved_valid");
            provider_repair_set_stage(out_result, "complete");
            goto publish;
        }
        if (ret != ESP_ERR_INVALID_RESPONSE) {
            goto done;
        }
        repair_invalid = true;
        snprintf(
            out_result->action, sizeof(out_result->action), "%s",
            "repairing_invalid");
    }
    out_result->before_bytes = before_size;

    provider_repair_set_stage(out_result, "stage_preflight");
    ret = provider_path_exists(
        D1L_MAP_PROVIDER_RECOVERY_STAGE_001_PATH, &stage_001_exists);
    if (ret != ESP_OK) {
        goto done;
    }
    if (stage_001_exists) {
        snprintf(
            out_result->stage_path, sizeof(out_result->stage_path), "%s",
            D1L_MAP_PROVIDER_RECOVERY_STAGE_001_PATH);
        provider_repair_set_stage(out_result, "stage_verify");
        ret = validate_default_provider_path(
            out_result->stage_path, verify, sizeof(verify), &validation);
        out_result->stage_default_exact = validation.bytes_exact;
        out_result->stage_hash_verified = validation.hash_verified;
        out_result->stage_parsed = validation.parsed;
        if (ret != ESP_OK) {
            goto done;
        }
        out_result->stage_reused = true;
    } else {
        ret = select_absent_enumerated_path(
            D1L_MAP_PROVIDER_STAGE_PATH_FORMAT,
            out_result->stage_path, sizeof(out_result->stage_path));
        if (ret != ESP_OK) {
            goto done;
        }
    }
    if (repair_invalid) {
        ret = select_absent_enumerated_path(
            D1L_MAP_PROVIDER_BACKUP_PATH_FORMAT,
            out_result->backup_path, sizeof(out_result->backup_path));
        if (ret != ESP_OK) {
            goto done;
        }
    }

    if (!out_result->stage_reused) {
        provider_repair_set_stage(out_result, "storage_quiesce");
        ret = provider_repair_quiesce_storage(
            out_result, &manager_quiesced, &retained_worker_quiesced);
        if (ret != ESP_OK) {
            goto done;
        }
        provider_repair_set_stage(out_result, "stage_create");
        d1l_provider_create_new_result_t stage_write = {0};
        ret = write_default_provider_stage(
            out_result->stage_path, &stage_write);
        out_result->stage_mutation_attempted = stage_write.attempted;
        out_result->stage_mutation_performed = stage_write.performed;
        out_result->stage_mutation_uncertain =
            stage_write.completion_uncertain;
        out_result->stage_create_new = stage_write.create_new;
        out_result->stage_path_fresh = stage_write.create_new;
        out_result->mutation_performed =
            out_result->mutation_performed || stage_write.performed;
        if (ret != ESP_OK) {
            goto done;
        }
        provider_repair_set_stage(out_result, "stage_verify");
        ret = validate_default_provider_path(
            out_result->stage_path, verify, sizeof(verify), &validation);
        out_result->stage_default_exact = validation.bytes_exact;
        out_result->stage_hash_verified = validation.hash_verified;
        out_result->stage_parsed = validation.parsed;
        if (ret != ESP_OK) {
            goto done;
        }
    }
    provider_repair_set_stage(out_result, "stage_verified");

    if (repair_invalid) {
        provider_repair_set_stage(out_result, "storage_quiesce");
        ret = provider_repair_quiesce_storage(
            out_result, &manager_quiesced, &retained_worker_quiesced);
        if (ret != ESP_OK) {
            goto done;
        }
        provider_repair_set_stage(out_result, "canonical_reverify");
        /*
         * Hold bounded storage-manager sequence ownership before this
         * immediate re-read so the exact invalid bytes cannot race another
         * bridge file user before the create-new backup copy.
         */
        out_result->canonical_before_reverify_hash_calculated =
            provider_sha256_hex(
                before, before_size,
                out_result->canonical_before_reverify_sha256);
        if (!out_result->canonical_before_reverify_hash_calculated) {
            ret = ESP_FAIL;
            goto done;
        }
        out_result->canonical_before_reverify_hash_matches_initial =
            out_result->canonical_initial_hash_calculated &&
            strcmp(
                out_result->canonical_initial_sha256,
                out_result->canonical_before_reverify_sha256) == 0;

        out_result->canonical_reverify_attempted = true;
        const esp_err_t reverify_ret = read_provider_path(
            D1L_MAP_PROVIDER_CONFIG_PATH, verify, sizeof(verify),
            &verify_size);
        out_result->canonical_reverify_io_result = reverify_ret;
        out_result->canonical_reverify_bytes = verify_size;
        if (reverify_ret == ESP_OK) {
            out_result->canonical_reverify_hash_calculated =
                provider_sha256_hex(
                    verify, verify_size,
                    out_result->canonical_reverify_sha256);
            if (!out_result->canonical_reverify_hash_calculated) {
                ret = ESP_FAIL;
                goto done;
            }
            const size_t shared_size =
                verify_size < before_size ? verify_size : before_size;
            for (size_t i = 0U; i < shared_size; ++i) {
                if ((uint8_t)before[i] == (uint8_t)verify[i]) {
                    continue;
                }
                out_result->canonical_reverify_first_mismatch_found = true;
                out_result->canonical_reverify_first_mismatch_index = i;
                out_result->canonical_reverify_first_mismatch_before_byte =
                    (uint8_t)before[i];
                out_result->canonical_reverify_first_mismatch_read_byte =
                    (uint8_t)verify[i];
                break;
            }
        }
        out_result->canonical_reverify_bytes_match =
            reverify_ret == ESP_OK &&
            verify_size == before_size &&
            memcmp(verify, before, before_size) == 0;
        if (!out_result->canonical_reverify_bytes_match) {
            ret = reverify_ret == ESP_OK ?
                ESP_ERR_INVALID_STATE : reverify_ret;
            goto done;
        }

        provider_repair_set_stage(out_result, "backup_create");
        d1l_provider_create_new_result_t backup_copy = {0};
        ret = write_provider_create_new(
            out_result->backup_path, before, before_size, &backup_copy);
        out_result->backup_copy_attempted = backup_copy.attempted;
        out_result->backup_copy_performed = backup_copy.performed;
        out_result->backup_copy_uncertain =
            backup_copy.completion_uncertain;
        out_result->backup_path_fresh = backup_copy.create_new;
        out_result->mutation_performed =
            out_result->mutation_performed || backup_copy.performed;
        if (backup_copy.create_new) {
            snprintf(
                out_result->preserved_backup_path,
                sizeof(out_result->preserved_backup_path), "%s",
                out_result->backup_path);
        }
        if (ret != ESP_OK) {
            goto done;
        }
        snprintf(
            out_result->action, sizeof(out_result->action), "%s",
            "repair_incomplete_forward");

        provider_repair_set_stage(out_result, "backup_verify");
        ret = verify_invalid_provider_copy(
            out_result->preserved_backup_path, before, before_size,
            verify, sizeof(verify), out_result);
        if (ret != ESP_OK) {
            goto done;
        }

        provider_repair_set_stage(out_result, "canonical_delete");
        out_result->canonical_delete_attempted = true;
        const esp_err_t delete_ret = d1l_rp2040_bridge_file_delete(
            D1L_MAP_PROVIDER_CONFIG_PATH, &file,
            D1L_MAP_PROVIDER_FILE_TIMEOUT_MS);
        const esp_err_t canonical_stat_ret = provider_path_exists(
            D1L_MAP_PROVIDER_CONFIG_PATH, &canonical_exists);
        if (canonical_stat_ret != ESP_OK) {
            out_result->canonical_delete_uncertain = true;
            ret = delete_ret != ESP_OK ? delete_ret : canonical_stat_ret;
            goto done;
        }
        if (canonical_exists) {
            ret = delete_ret != ESP_OK ? delete_ret : ESP_ERR_INVALID_STATE;
            goto done;
        }
        out_result->canonical_delete_performed = true;
        out_result->canonical_absent_before_final_rename = true;
        out_result->mutation_performed = true;
        out_result->canonical_missing_recoverable = true;
        snprintf(
            out_result->action, sizeof(out_result->action), "%s",
            "canonical_missing_recoverable");
    } else {
        /*
         * A previous forward attempt already preserved the invalid canonical
         * and left the canonical path absent. Re-verify that preserved backup
         * and the absence before resuming; never move the backup back.
         */
        provider_repair_set_stage(out_result, "backup_verify");
        ret = verify_invalid_provider_copy(
            out_result->preserved_backup_path, before, before_size,
            verify, sizeof(verify), out_result);
        if (ret != ESP_OK) {
            goto done;
        }
        provider_repair_set_stage(out_result, "storage_quiesce");
        ret = provider_repair_quiesce_storage(
            out_result, &manager_quiesced, &retained_worker_quiesced);
        if (ret != ESP_OK) {
            goto done;
        }
        provider_repair_set_stage(
            out_result, "canonical_absence_verify");
        ret = provider_path_exists(
            D1L_MAP_PROVIDER_CONFIG_PATH, &canonical_exists);
        if (ret != ESP_OK || canonical_exists) {
            ret = ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
            goto done;
        }
        out_result->canonical_absent_before_final_rename = true;
        out_result->canonical_missing_recoverable = true;
        snprintf(
            out_result->action, sizeof(out_result->action), "%s",
            "canonical_missing_recoverable");
    }

    provider_repair_set_stage(out_result, "final_rename");
    out_result->final_rename_attempted = true;
    ret = d1l_rp2040_bridge_file_rename(
        out_result->stage_path, D1L_MAP_PROVIDER_CONFIG_PATH, false, &file,
        D1L_MAP_PROVIDER_FILE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        const esp_err_t stage_stat_ret = provider_path_exists(
            out_result->stage_path, &stage_exists);
        const esp_err_t canonical_stat_ret = provider_path_exists(
            D1L_MAP_PROVIDER_CONFIG_PATH, &canonical_exists);
        if (stage_stat_ret == ESP_OK && canonical_stat_ret == ESP_OK) {
            out_result->stage_present_after_failure = stage_exists;
            out_result->canonical_missing_recoverable =
                stage_exists && !canonical_exists &&
                out_result->backup_preserved;
            out_result->final_rename_uncertain =
                !out_result->canonical_missing_recoverable;
            if (out_result->canonical_missing_recoverable) {
                snprintf(
                    out_result->action, sizeof(out_result->action), "%s",
                    "canonical_missing_recoverable");
            }
        } else {
            out_result->final_rename_uncertain = true;
        }
        goto done;
    }
    out_result->final_rename_performed = true;
    out_result->mutation_performed = true;
    out_result->canonical_missing_recoverable = false;
    snprintf(
        out_result->action, sizeof(out_result->action), "%s",
        "repair_incomplete_forward");

    provider_repair_set_stage(out_result, "final_validate");
    ret = validate_default_provider_path(
        D1L_MAP_PROVIDER_CONFIG_PATH, verify, sizeof(verify), &validation);
    if (ret != ESP_OK) {
        goto done;
    }

    provider = validation.provider;
    out_result->final_valid = true;
    out_result->final_builtin_exact = true;
    out_result->final_bytes = validation.bytes;
    snprintf(
        out_result->source_id, sizeof(out_result->source_id), "%s",
        provider.source_id);
    snprintf(
        out_result->action, sizeof(out_result->action), "%s",
        out_result->recovery_resumed ?
            "resumed_missing_canonical" : "repaired_invalid");
    provider_repair_set_stage(out_result, "complete");

publish:
    portENTER_CRITICAL(&s_provider_lock);
    s_provider = provider;
    s_provider_initialized = true;
    portEXIT_CRITICAL(&s_provider_lock);
    ret = ESP_OK;

done:
    if (retained_worker_quiesced) {
        d1l_route_store_worker_quiesce_end();
        out_result->retained_worker_quiesce_released = true;
    }
    if (manager_quiesced) {
        d1l_storage_manager_quiesce_end();
        out_result->storage_manager_resumed = true;
        out_result->storage_manager_quiesce_released = true;
    }
    memset(before, 0, sizeof(before));
    memset(verify, 0, sizeof(verify));
    provider_io_release();
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
