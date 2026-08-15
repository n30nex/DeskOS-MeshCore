#include "observer_manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app/release_profile.h"
#include "app/settings_model.h"
#include "comms/connectivity_manager.h"
#include "d1l_config.h"
#include "diagnostics/event_log.h"
#include "ed_25519.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mesh/meshcore_service.h"
#include "mesh/meshcore_wire.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "platform/time_service.h"

#define D1L_OBSERVER_NAMESPACE "d1l_observer"
#define D1L_OBSERVER_DEFAULT_REGION "YKF"
#define D1L_OBSERVER_PRIMARY_URI "wss://mqtt1.meshcore.ca:443/mqtt"
#define D1L_OBSERVER_SECONDARY_URI "wss://mqtt2.meshcore.ca:443/mqtt"
#define D1L_OBSERVER_PRIMARY_AUDIENCE "mqtt1.meshcore.ca"
#define D1L_OBSERVER_SECONDARY_AUDIENCE "mqtt2.meshcore.ca"
#define D1L_OBSERVER_QUEUE_CAPACITY 8U
#define D1L_OBSERVER_PACKET_QUEUE_CAPACITY 8U
#define D1L_OBSERVER_PAYLOAD_LEN 1024U
#define D1L_OBSERVER_TASK_STACK_BYTES 8192U
#define D1L_OBSERVER_CLIENT_STACK_BYTES 6144U
#define D1L_OBSERVER_PUBLISH_INTERVAL_MS 300000U
#define D1L_OBSERVER_BACKOFF_MS 5000U
#define D1L_OBSERVER_LOOP_MS 200U
#define D1L_OBSERVER_ENDPOINT_START_GAP_MS 12000U
#define D1L_OBSERVER_TOKEN_LIFETIME_SEC 3600U
#define D1L_OBSERVER_TOKEN_RENEW_SEC 2700U
#define D1L_OBSERVER_PRIMARY_INDEX 0U
#define D1L_OBSERVER_SECONDARY_INDEX 1U
#define D1L_OBSERVER_CUSTOM_INDEX 2U

typedef struct {
    char uri[D1L_OBSERVER_URI_LEN];
    char topic[D1L_OBSERVER_TOPIC_LEN];
    char username[D1L_OBSERVER_USERNAME_LEN];
    char password[D1L_OBSERVER_PASSWORD_LEN];
    char region[D1L_OBSERVER_REGION_LEN];
    bool include_location;
    bool configured;
} d1l_observer_config_t;

typedef struct {
    uint32_t sequence;
    uint8_t pending_mask;
    bool retain;
    char suffix[12];
    char payload[D1L_OBSERVER_PAYLOAD_LEN];
} d1l_observer_queue_entry_t;

typedef struct {
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET];
    uint8_t raw_len;
    int16_t rssi_dbm;
    int8_t snr_quarter_db;
} d1l_observer_packet_entry_t;

typedef struct {
    uint8_t index;
    const char *uri;
    const char *audience;
    esp_mqtt_client_handle_t client;
    bool connected;
    uint32_t backoff_until_ms;
    uint32_t token_issued_at;
    uint32_t inflight_sequence;
    int inflight_message_id;
    esp_err_t last_error;
    d1l_observer_endpoint_diagnostic_t diagnostic;
    char username[D1L_OBSERVER_USERNAME_LEN];
    char password[D1L_OBSERVER_PASSWORD_LEN];
} d1l_observer_endpoint_t;

static const char *TAG = "d1l_observer";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_observer_config_t s_config;
static d1l_observer_queue_entry_t
    s_queue[D1L_OBSERVER_QUEUE_CAPACITY] EXT_RAM_BSS_ATTR;
static size_t s_queue_head;
static size_t s_queue_count;
static uint32_t s_queue_sequence = 1U;
static d1l_observer_packet_entry_t
    s_packet_queue[D1L_OBSERVER_PACKET_QUEUE_CAPACITY] EXT_RAM_BSS_ATTR;
static size_t s_packet_queue_head;
static size_t s_packet_queue_count;
static d1l_observer_status_t s_status = {
    .state = D1L_OBSERVER_STATE_DISABLED,
    .queue_capacity = D1L_OBSERVER_QUEUE_CAPACITY +
                      D1L_OBSERVER_PACKET_QUEUE_CAPACITY,
};
static d1l_observer_endpoint_t s_endpoints[D1L_OBSERVER_BROKER_COUNT];
static SemaphoreHandle_t s_client_lock;
static TaskHandle_t s_task;
static uint32_t s_last_periodic_ms;
static uint32_t s_last_endpoint_start_ms;
static char s_public_key_hex[65];

static bool take_client_lock(void)
{
    return s_client_lock &&
           xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(5000U)) == pdTRUE;
}

static void give_client_lock(void)
{
    if (s_client_lock) {
        xSemaphoreGive(s_client_lock);
    }
}

static void secure_zero(void *value, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)value;
    while (bytes && size-- > 0U) {
        *bytes++ = 0U;
    }
}

static bool text_valid(const char *value, size_t capacity, bool allow_empty)
{
    if (!value) {
        return allow_empty;
    }
    const size_t length = strnlen(value, capacity);
    if (length >= capacity || (!allow_empty && length == 0U)) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        const unsigned char ch = (unsigned char)value[i];
        if (ch < 0x20U || ch == 0x7fU) {
            return false;
        }
    }
    return true;
}

static bool secure_mqtt_uri_valid(const char *uri)
{
    if (!text_valid(uri, D1L_OBSERVER_URI_LEN, false)) {
        return false;
    }
    const char *after_scheme = NULL;
    if (strncmp(uri, "mqtts://", 8U) == 0) {
        after_scheme = uri + 8U;
    } else if (strncmp(uri, "wss://", 6U) == 0) {
        after_scheme = uri + 6U;
    }
    return after_scheme && after_scheme[0] != '\0';
}

static bool region_valid(const char *iata)
{
    if (!iata || strlen(iata) != 3U) {
        return false;
    }
    for (size_t i = 0U; i < 3U; ++i) {
        if (iata[i] < 'A' || iata[i] > 'Z') {
            return false;
        }
    }
    return true;
}

static void public_broker_name(const char *uri, char *out, size_t out_size)
{
    if (!out || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    if (!uri) {
        return;
    }
    const char *host = strstr(uri, "://");
    host = host ? host + 3U : uri;
    const char *end = host;
    while (*end && *end != ':' && *end != '/' && *end != '?' &&
           *end != '#') {
        end++;
    }
    snprintf(out, out_size, "%.*s", (int)(end - host), host);
}

static void bytes_to_hex(const uint8_t *bytes, size_t length, char *out,
                         bool uppercase)
{
    static const char lower[] = "0123456789abcdef";
    static const char upper[] = "0123456789ABCDEF";
    const char *alphabet = uppercase ? upper : lower;
    for (size_t i = 0U; i < length; ++i) {
        out[i * 2U] = alphabet[(bytes[i] >> 4U) & 0x0fU];
        out[i * 2U + 1U] = alphabet[bytes[i] & 0x0fU];
    }
    out[length * 2U] = '\0';
}

static bool base64url_encode(const uint8_t *input, size_t input_len,
                             char *out, size_t out_size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const size_t encoded_len = (input_len * 4U + 2U) / 3U;
    if (!input || !out || out_size <= encoded_len) {
        return false;
    }
    size_t source = 0U;
    size_t dest = 0U;
    while (source + 3U <= input_len) {
        const uint32_t value = ((uint32_t)input[source] << 16U) |
                               ((uint32_t)input[source + 1U] << 8U) |
                               input[source + 2U];
        out[dest++] = alphabet[(value >> 18U) & 0x3fU];
        out[dest++] = alphabet[(value >> 12U) & 0x3fU];
        out[dest++] = alphabet[(value >> 6U) & 0x3fU];
        out[dest++] = alphabet[value & 0x3fU];
        source += 3U;
    }
    const size_t remaining = input_len - source;
    if (remaining == 1U) {
        const uint32_t value = (uint32_t)input[source] << 16U;
        out[dest++] = alphabet[(value >> 18U) & 0x3fU];
        out[dest++] = alphabet[(value >> 12U) & 0x3fU];
    } else if (remaining == 2U) {
        const uint32_t value = ((uint32_t)input[source] << 16U) |
                               ((uint32_t)input[source + 1U] << 8U);
        out[dest++] = alphabet[(value >> 18U) & 0x3fU];
        out[dest++] = alphabet[(value >> 12U) & 0x3fU];
        out[dest++] = alphabet[(value >> 6U) & 0x3fU];
    }
    out[dest] = '\0';
    return dest == encoded_len;
}

static bool current_utc(uint32_t *out_epoch, char *timestamp,
                        size_t timestamp_size, char *clock,
                        size_t clock_size, char *date, size_t date_size)
{
    d1l_time_service_status_t status = {0};
    d1l_time_service_status(&status);
    if (!status.display_time_valid || status.clock.wall_epoch_sec <= 0 ||
        (uint64_t)status.clock.wall_epoch_sec > UINT32_MAX) {
        return false;
    }
    const time_t epoch = (time_t)status.clock.wall_epoch_sec;
    struct tm utc = {0};
    if (!gmtime_r(&epoch, &utc)) {
        return false;
    }
    if (out_epoch) {
        *out_epoch = (uint32_t)status.clock.wall_epoch_sec;
    }
    if (timestamp && timestamp_size > 0U &&
        strftime(timestamp, timestamp_size, "%Y-%m-%dT%H:%M:%S+00:00",
                 &utc) == 0U) {
        return false;
    }
    if (clock && clock_size > 0U &&
        strftime(clock, clock_size, "%H:%M:%S", &utc) == 0U) {
        return false;
    }
    if (date && date_size > 0U &&
        strftime(date, date_size, "%d/%m/%Y", &utc) == 0U) {
        return false;
    }
    return true;
}

static bool json_escape(const char *input, char *out, size_t out_size)
{
    if (!input || !out || out_size == 0U) {
        return false;
    }
    size_t dest = 0U;
    for (size_t source = 0U; input[source] != '\0'; ++source) {
        const unsigned char ch = (unsigned char)input[source];
        if (ch < 0x20U || ch == 0x7fU) {
            continue;
        }
        if (ch == '"' || ch == '\\') {
            if (dest + 2U >= out_size) {
                return false;
            }
            out[dest++] = '\\';
        } else if (dest + 1U >= out_size) {
            return false;
        }
        out[dest++] = (char)ch;
    }
    out[dest] = '\0';
    return true;
}

static esp_err_t observer_create_token(const char *audience,
                                       char *username, size_t username_size,
                                       char *token, size_t token_size,
                                       uint32_t *out_issued_at)
{
    if (!audience || !username || !token || !out_issued_at) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t now = 0U;
    if (!current_utc(&now, NULL, 0U, NULL, 0U, NULL, 0U)) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_settings_identity_secret_t identity = {0};
    esp_err_t ret = d1l_settings_identity_secret_snapshot(&identity);
    if (ret != ESP_OK || !identity.identity_ready) {
        d1l_settings_identity_secret_wipe(&identity);
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    char public_key[65] = {0};
    bytes_to_hex(identity.identity_public_key,
                 sizeof(identity.identity_public_key), public_key, true);
    if (snprintf(username, username_size, "v1_%s", public_key) >=
        (int)username_size) {
        d1l_settings_identity_secret_wipe(&identity);
        return ESP_ERR_INVALID_SIZE;
    }
    static const char header_json[] = "{\"alg\":\"Ed25519\",\"typ\":\"JWT\"}";
    char payload_json[192] = {0};
    const int payload_len = snprintf(
        payload_json, sizeof(payload_json),
        "{\"publicKey\":\"%s\",\"iat\":%lu,\"exp\":%lu,\"aud\":\"%s\"}",
        public_key, (unsigned long)now,
        (unsigned long)(now + D1L_OBSERVER_TOKEN_LIFETIME_SEC), audience);
    char header_encoded[64] = {0};
    char payload_encoded[256] = {0};
    char signing_input[336] = {0};
    uint8_t signature[64] = {0};
    char signature_hex[129] = {0};
    if (payload_len <= 0 || payload_len >= (int)sizeof(payload_json) ||
        !base64url_encode((const uint8_t *)header_json,
                          strlen(header_json), header_encoded,
                          sizeof(header_encoded)) ||
        !base64url_encode((const uint8_t *)payload_json,
                          (size_t)payload_len, payload_encoded,
                          sizeof(payload_encoded)) ||
        snprintf(signing_input, sizeof(signing_input), "%s.%s",
                 header_encoded, payload_encoded) >=
            (int)sizeof(signing_input)) {
        ret = ESP_ERR_INVALID_SIZE;
    } else {
        ed25519_sign(signature, (const uint8_t *)signing_input,
                     strlen(signing_input), identity.identity_public_key,
                     identity.identity_private_key);
        bytes_to_hex(signature, sizeof(signature), signature_hex, false);
        if (snprintf(token, token_size, "%s.%s", signing_input,
                     signature_hex) >= (int)token_size) {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            *out_issued_at = now;
            ret = ESP_OK;
        }
    }
    secure_zero(payload_json, sizeof(payload_json));
    secure_zero(signing_input, sizeof(signing_input));
    secure_zero(signature, sizeof(signature));
    secure_zero(signature_hex, sizeof(signature_hex));
    d1l_settings_identity_secret_wipe(&identity);
    return ret;
}

static esp_err_t refresh_public_identity(void)
{
    d1l_settings_identity_secret_t identity = {0};
    const esp_err_t ret = d1l_settings_identity_secret_snapshot(&identity);
    if (ret == ESP_OK && identity.identity_ready) {
        bytes_to_hex(identity.identity_public_key,
                     sizeof(identity.identity_public_key),
                     s_public_key_hex, true);
    } else {
        s_public_key_hex[0] = '\0';
    }
    d1l_settings_identity_secret_wipe(&identity);
    return s_public_key_hex[0] ? ESP_OK :
        (ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret);
}

static esp_err_t load_config(d1l_observer_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    secure_zero(out, sizeof(*out));
    snprintf(out->region, sizeof(out->region), "%s",
             D1L_OBSERVER_DEFAULT_REGION);
    nvs_handle_t handle = 0U;
    esp_err_t ret = nvs_open(D1L_OBSERVER_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    size_t region_size = sizeof(out->region);
    const esp_err_t region_ret =
        nvs_get_str(handle, "region", out->region, &region_size);
    if (region_ret != ESP_OK && region_ret != ESP_ERR_NVS_NOT_FOUND) {
        ret = region_ret;
    }
    size_t uri_size = sizeof(out->uri);
    const esp_err_t uri_ret = nvs_get_str(handle, "uri", out->uri, &uri_size);
    if (ret == ESP_OK && uri_ret == ESP_OK) {
        size_t topic_size = sizeof(out->topic);
        size_t username_size = sizeof(out->username);
        size_t password_size = sizeof(out->password);
        uint8_t include_location = 0U;
        ret = nvs_get_str(handle, "topic", out->topic, &topic_size);
        if (ret == ESP_OK) {
            const esp_err_t value_ret =
                nvs_get_str(handle, "user", out->username, &username_size);
            if (value_ret != ESP_OK && value_ret != ESP_ERR_NVS_NOT_FOUND) {
                ret = value_ret;
            }
        }
        if (ret == ESP_OK) {
            const esp_err_t value_ret =
                nvs_get_str(handle, "pass", out->password, &password_size);
            if (value_ret != ESP_OK && value_ret != ESP_ERR_NVS_NOT_FOUND) {
                ret = value_ret;
            }
        }
        if (ret == ESP_OK) {
            const esp_err_t value_ret =
                nvs_get_u8(handle, "location", &include_location);
            if (value_ret != ESP_OK && value_ret != ESP_ERR_NVS_NOT_FOUND) {
                ret = value_ret;
            }
        }
        out->include_location = include_location != 0U;
        out->configured = ret == ESP_OK;
    } else if (ret == ESP_OK && uri_ret != ESP_ERR_NVS_NOT_FOUND) {
        ret = uri_ret;
    }
    nvs_close(handle);
    if (ret != ESP_OK || !region_valid(out->region) ||
        (out->configured &&
         (!secure_mqtt_uri_valid(out->uri) ||
          !text_valid(out->topic, sizeof(out->topic), false) ||
          !text_valid(out->username, sizeof(out->username), true) ||
          !text_valid(out->password, sizeof(out->password), true)))) {
        secure_zero(out, sizeof(*out));
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    return ESP_OK;
}

static esp_err_t save_config(const d1l_observer_config_t *config)
{
    if (!config || !config->configured || !region_valid(config->region)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0U;
    esp_err_t ret = nvs_open(D1L_OBSERVER_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_OK) ret = nvs_set_str(handle, "region", config->region);
    if (ret == ESP_OK) ret = nvs_set_str(handle, "uri", config->uri);
    if (ret == ESP_OK) ret = nvs_set_str(handle, "topic", config->topic);
    if (ret == ESP_OK) ret = nvs_set_str(handle, "user", config->username);
    if (ret == ESP_OK) ret = nvs_set_str(handle, "pass", config->password);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, "location",
                         config->include_location ? 1U : 0U);
    }
    if (ret == ESP_OK) ret = nvs_commit(handle);
    if (handle != 0U) nvs_close(handle);
    return ret;
}

static esp_err_t save_region(const char *region)
{
    nvs_handle_t handle = 0U;
    esp_err_t ret = nvs_open(D1L_OBSERVER_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_OK) ret = nvs_set_str(handle, "region", region);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    if (handle != 0U) nvs_close(handle);
    return ret;
}

static uint8_t active_endpoint_mask_locked(void)
{
    uint8_t mask = (1U << D1L_OBSERVER_PRIMARY_INDEX) |
                   (1U << D1L_OBSERVER_SECONDARY_INDEX);
    if (s_config.configured) {
        mask |= (1U << D1L_OBSERVER_CUSTOM_INDEX);
    }
    return mask;
}

static void prune_queue_locked(void)
{
    while (s_queue_count > 0U &&
           s_queue[s_queue_head].pending_mask == 0U) {
        secure_zero(&s_queue[s_queue_head], sizeof(s_queue[s_queue_head]));
        s_queue_head = (s_queue_head + 1U) % D1L_OBSERVER_QUEUE_CAPACITY;
        s_queue_count--;
    }
    s_status.queued =
        (uint32_t)(s_queue_count + s_packet_queue_count);
}

static void queue_payload(const char *suffix, const char *payload, bool retain)
{
    if (!suffix || !payload || payload[0] == '\0') {
        return;
    }
    const size_t payload_length = strnlen(payload, D1L_OBSERVER_PAYLOAD_LEN);
    if (payload_length >= D1L_OBSERVER_PAYLOAD_LEN) {
        portENTER_CRITICAL(&s_lock);
        s_status.dropped_oldest++;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    portENTER_CRITICAL(&s_lock);
    const uint8_t pending_mask = active_endpoint_mask_locked();
    if (pending_mask == 0U) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (s_queue_count == D1L_OBSERVER_QUEUE_CAPACITY) {
        secure_zero(&s_queue[s_queue_head], sizeof(s_queue[s_queue_head]));
        s_queue_head = (s_queue_head + 1U) % D1L_OBSERVER_QUEUE_CAPACITY;
        s_queue_count--;
        s_status.dropped_oldest++;
    }
    const size_t slot =
        (s_queue_head + s_queue_count) % D1L_OBSERVER_QUEUE_CAPACITY;
    s_queue[slot].sequence = s_queue_sequence++;
    if (s_queue_sequence == 0U) s_queue_sequence = 1U;
    s_queue[slot].pending_mask = pending_mask;
    s_queue[slot].retain = retain;
    snprintf(s_queue[slot].suffix, sizeof(s_queue[slot].suffix), "%s", suffix);
    memcpy(s_queue[slot].payload, payload, payload_length + 1U);
    s_queue_count++;
    s_status.queued =
        (uint32_t)(s_queue_count + s_packet_queue_count);
    s_status.queued_total++;
    portEXIT_CRITICAL(&s_lock);
}

static bool peek_payload_for_endpoint(uint8_t endpoint_index,
                                      d1l_observer_queue_entry_t *out)
{
    if (!out || endpoint_index >= D1L_OBSERVER_BROKER_COUNT) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    bool found = false;
    for (size_t offset = 0U; offset < s_queue_count; ++offset) {
        const size_t slot =
            (s_queue_head + offset) % D1L_OBSERVER_QUEUE_CAPACITY;
        if ((s_queue[slot].pending_mask & (1U << endpoint_index)) != 0U) {
            *out = s_queue[slot];
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}

static void mark_payload_acknowledged(uint32_t sequence,
                                      uint8_t endpoint_index)
{
    portENTER_CRITICAL(&s_lock);
    for (size_t offset = 0U; offset < s_queue_count; ++offset) {
        const size_t slot =
            (s_queue_head + offset) % D1L_OBSERVER_QUEUE_CAPACITY;
        if (s_queue[slot].sequence == sequence) {
            s_queue[slot].pending_mask &= (uint8_t)~(1U << endpoint_index);
            break;
        }
    }
    prune_queue_locked();
    portEXIT_CRITICAL(&s_lock);
}

static void refresh_aggregate_state(bool enabled, bool wifi_connected)
{
    portENTER_CRITICAL(&s_lock);
    const uint8_t active_mask = active_endpoint_mask_locked();
    uint8_t active = 0U;
    uint8_t connected = 0U;
    esp_err_t last_error = ESP_OK;
    for (uint8_t i = 0U; i < D1L_OBSERVER_BROKER_COUNT; ++i) {
        if ((active_mask & (1U << i)) == 0U) continue;
        active++;
        if (s_endpoints[i].connected) connected++;
        if (s_endpoints[i].last_error != ESP_OK) {
            last_error = s_endpoints[i].last_error;
        }
    }
    s_status.enabled = enabled;
    s_status.configured = s_public_key_hex[0] != '\0';
    s_status.connected = connected > 0U;
    s_status.broker_count = active;
    s_status.connected_brokers = connected;
    s_status.primary_connected = s_endpoints[0].connected;
    s_status.secondary_connected = s_endpoints[1].connected;
    s_status.custom_configured = s_config.configured;
    s_status.custom_connected = s_endpoints[2].connected;
    s_status.last_error = last_error;
    if (!enabled) {
        s_status.state = D1L_OBSERVER_STATE_DISABLED;
    } else if (!s_status.configured) {
        s_status.state = D1L_OBSERVER_STATE_NOT_CONFIGURED;
    } else if (!wifi_connected) {
        s_status.state = D1L_OBSERVER_STATE_WAITING_FOR_WIFI;
    } else if (connected == active && active > 0U) {
        s_status.state = D1L_OBSERVER_STATE_CONNECTED;
    } else if (connected > 0U) {
        s_status.state = D1L_OBSERVER_STATE_CONNECTING;
    } else if (last_error != ESP_OK) {
        s_status.state = D1L_OBSERVER_STATE_BACKOFF;
    } else {
        s_status.state = D1L_OBSERVER_STATE_CONNECTING;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)base;
    d1l_observer_endpoint_t *endpoint =
        (d1l_observer_endpoint_t *)handler_args;
    if (!endpoint) return;
    const esp_mqtt_event_handle_t event =
        (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        portENTER_CRITICAL(&s_lock);
        endpoint->connected = true;
        endpoint->last_error = ESP_OK;
        memset(&endpoint->diagnostic, 0, sizeof(endpoint->diagnostic));
        endpoint->backoff_until_ms = 0U;
        s_status.reconnects++;
        portEXIT_CRITICAL(&s_lock);
        d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "observer",
                             "connected", "secure broker connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        portENTER_CRITICAL(&s_lock);
        endpoint->connected = false;
        if (endpoint->last_error == ESP_OK) {
            endpoint->last_error = ESP_ERR_INVALID_STATE;
        }
        portEXIT_CRITICAL(&s_lock);
        break;
    case MQTT_EVENT_PUBLISHED: {
        uint32_t acknowledged_sequence = 0U;
        portENTER_CRITICAL(&s_lock);
        s_status.acknowledged_total++;
        if (event && event->msg_id > 0) {
            s_status.last_message_id = (uint32_t)event->msg_id;
            if (endpoint->inflight_sequence != 0U &&
                endpoint->inflight_message_id == event->msg_id) {
                acknowledged_sequence = endpoint->inflight_sequence;
                endpoint->inflight_sequence = 0U;
                endpoint->inflight_message_id = 0;
            }
        }
        portEXIT_CRITICAL(&s_lock);
        if (acknowledged_sequence != 0U) {
            mark_payload_acknowledged(acknowledged_sequence,
                                      endpoint->index);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        portENTER_CRITICAL(&s_lock);
        endpoint->last_error = ESP_FAIL;
        if (event && event->error_handle) {
            endpoint->diagnostic = (d1l_observer_endpoint_diagnostic_t) {
                .error_type = event->error_handle->error_type,
                .connect_return_code =
                    event->error_handle->connect_return_code,
                .tls_esp_error =
                    event->error_handle->esp_tls_last_esp_err,
                .tls_stack_error =
                    event->error_handle->esp_tls_stack_err,
                .socket_errno =
                    event->error_handle->esp_transport_sock_errno,
            };
        }
        portEXIT_CRITICAL(&s_lock);
        d1l_event_log_append(D1L_EVENT_LOG_LEVEL_ERROR, "observer",
                             "error", "secure MQTT transport error");
        break;
    default:
        break;
    }
}

static void stop_endpoint_locked(d1l_observer_endpoint_t *endpoint)
{
    if (!endpoint || !endpoint->client) return;
    (void)esp_mqtt_client_stop(endpoint->client);
    (void)esp_mqtt_client_destroy(endpoint->client);
    endpoint->client = NULL;
    endpoint->connected = false;
    endpoint->token_issued_at = 0U;
    endpoint->inflight_sequence = 0U;
    endpoint->inflight_message_id = 0;
    secure_zero(endpoint->username, sizeof(endpoint->username));
    secure_zero(endpoint->password, sizeof(endpoint->password));
}

static void stop_endpoint(uint8_t index)
{
    if (index >= D1L_OBSERVER_BROKER_COUNT || !take_client_lock()) return;
    stop_endpoint_locked(&s_endpoints[index]);
    give_client_lock();
}

static void stop_all_clients(void)
{
    if (!take_client_lock()) return;
    for (size_t i = 0U; i < D1L_OBSERVER_BROKER_COUNT; ++i) {
        stop_endpoint_locked(&s_endpoints[i]);
    }
    give_client_lock();
}

static bool endpoint_client_exists(uint8_t index)
{
    if (index >= D1L_OBSERVER_BROKER_COUNT || !take_client_lock()) {
        return true;
    }
    const bool exists = s_endpoints[index].client != NULL;
    give_client_lock();
    return exists;
}

static esp_err_t start_endpoint(uint8_t index, uint32_t now_ms)
{
    if (index >= D1L_OBSERVER_BROKER_COUNT || !take_client_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    d1l_observer_endpoint_t *endpoint = &s_endpoints[index];
    if (endpoint->client) {
        give_client_lock();
        return ESP_OK;
    }
    const char *uri = endpoint->uri;
    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&s_lock);
    endpoint->last_error = ESP_OK;
    memset(&endpoint->diagnostic, 0, sizeof(endpoint->diagnostic));
    portEXIT_CRITICAL(&s_lock);
    if (index == D1L_OBSERVER_CUSTOM_INDEX) {
        char audience[D1L_OBSERVER_URI_LEN] = {0};
        portENTER_CRITICAL(&s_lock);
        snprintf(endpoint->username, sizeof(endpoint->username), "%s",
                 s_config.username);
        snprintf(endpoint->password, sizeof(endpoint->password), "%s",
                 s_config.password);
        public_broker_name(s_config.uri, audience, sizeof(audience));
        portEXIT_CRITICAL(&s_lock);
        if (endpoint->username[0] == '\0' && endpoint->password[0] == '\0') {
            ret = observer_create_token(
                audience, endpoint->username, sizeof(endpoint->username),
                endpoint->password, sizeof(endpoint->password),
                &endpoint->token_issued_at);
        }
    } else {
        ret = observer_create_token(
            endpoint->audience, endpoint->username,
            sizeof(endpoint->username), endpoint->password,
            sizeof(endpoint->password), &endpoint->token_issued_at);
    }
    if (ret != ESP_OK) {
        endpoint->last_error = ret;
        endpoint->backoff_until_ms = now_ms + D1L_OBSERVER_BACKOFF_MS;
        secure_zero(endpoint->username, sizeof(endpoint->username));
        secure_zero(endpoint->password, sizeof(endpoint->password));
        give_client_lock();
        return ret;
    }
    const esp_mqtt_client_config_t config = {
        .broker = {
            .address.uri = uri,
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
        },
        .credentials = {
            .username = endpoint->username[0] ? endpoint->username : NULL,
            .authentication.password =
                endpoint->password[0] ? endpoint->password : NULL,
        },
        .session = {.keepalive = 60, .disable_clean_session = false},
        .network = {
            .reconnect_timeout_ms = D1L_OBSERVER_BACKOFF_MS,
            .timeout_ms = 10000,
            .disable_auto_reconnect = false,
        },
        .task = {.priority = 3, .stack_size = D1L_OBSERVER_CLIENT_STACK_BYTES},
        .buffer = {.size = 2048, .out_size = 2048},
        .outbox.limit = 8192,
    };
    endpoint->client = esp_mqtt_client_init(&config);
    if (!endpoint->client) {
        ret = ESP_ERR_NO_MEM;
    } else {
        ret = esp_mqtt_client_register_event(
            endpoint->client, ESP_EVENT_ANY_ID, mqtt_event_handler, endpoint);
        if (ret == ESP_OK) ret = esp_mqtt_client_start(endpoint->client);
    }
    if (ret != ESP_OK) {
        if (endpoint->client) {
            (void)esp_mqtt_client_destroy(endpoint->client);
            endpoint->client = NULL;
        }
        endpoint->last_error = ret;
        endpoint->backoff_until_ms = now_ms + D1L_OBSERVER_BACKOFF_MS;
        secure_zero(endpoint->username, sizeof(endpoint->username));
        secure_zero(endpoint->password, sizeof(endpoint->password));
    }
    give_client_lock();
    return ret;
}

static int publish_to_endpoint(uint8_t index, const char *topic,
                               const char *payload, bool retain)
{
    if (index >= D1L_OBSERVER_BROKER_COUNT || !topic || !payload ||
        !take_client_lock()) {
        return -1;
    }
    d1l_observer_endpoint_t *endpoint = &s_endpoints[index];
    const int message_id = endpoint->client && endpoint->connected ?
        esp_mqtt_client_enqueue(endpoint->client, topic, payload, 0, 1,
                                retain ? 1 : 0, true) : -1;
    give_client_lock();
    return message_id;
}

static bool observer_enabled(void)
{
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    return settings.observer_enabled &&
        d1l_release_feature_available(D1L_RELEASE_FEATURE_OBSERVER_MQTT);
}

static bool observer_network_continue(void *context)
{
    (void)context;
    d1l_connectivity_status_t connectivity = {0};
    d1l_connectivity_status(&connectivity);
    return observer_enabled() && connectivity.wifi_connected;
}

static void note_endpoint_error(esp_err_t error, uint32_t retry_at_ms)
{
    portENTER_CRITICAL(&s_lock);
    const uint8_t active_mask = active_endpoint_mask_locked();
    for (uint8_t i = 0U; i < D1L_OBSERVER_BROKER_COUNT; ++i) {
        if ((active_mask & (1U << i)) == 0U) continue;
        s_endpoints[i].last_error = error;
        s_endpoints[i].backoff_until_ms = retry_at_ms;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void enqueue_status_payload(void)
{
    char timestamp[32] = {0};
    if (!current_utc(NULL, timestamp, sizeof(timestamp), NULL, 0U,
                     NULL, 0U)) {
        return;
    }
    d1l_meshcore_service_status_t mesh = d1l_meshcore_service_status();
    d1l_connectivity_status_t connectivity = {0};
    d1l_connectivity_status(&connectivity);
    d1l_observer_status_t observer = {0};
    d1l_observer_status(&observer);
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    char origin[2U * D1L_NODE_NAME_LEN] = {0};
    if (!json_escape(settings.node_name, origin, sizeof(origin))) {
        snprintf(origin, sizeof(origin), "DeskOS D1L");
    }
    bool include_location = false;
    portENTER_CRITICAL(&s_lock);
    include_location = s_config.include_location;
    portEXIT_CRITICAL(&s_lock);
    char location[112] = {0};
    if (include_location && settings.map_location_set) {
        snprintf(location, sizeof(location),
                 ",\"location\":{\"source\":\"manual_or_companion\","
                 "\"lat_e7\":%ld,\"lon_e7\":%ld}",
                 (long)settings.map_lat_e7, (long)settings.map_lon_e7);
    }
    char payload[D1L_OBSERVER_PAYLOAD_LEN] = {0};
    const int written = snprintf(
        payload, sizeof(payload),
        "{\"status\":\"online\",\"timestamp\":\"%s\","
        "\"origin\":\"%s\",\"origin_id\":\"%s\","
        "\"model\":\"seeed_indicator_d1l\","
        "\"firmware_version\":\"%s\",\"client_version\":\"deskos/%s\","
        "\"repeat\":\"off\",\"stats\":{\"uptime_secs\":%lu,"
        "\"packets_sent\":%lu,\"packets_received\":%lu,"
        "\"errors\":%lu,\"queue_len\":%lu},"
        "\"wifi_rssi_dbm\":%d%s}",
        timestamp, origin, s_public_key_hex, D1L_FIRMWARE_VERSION,
        D1L_FIRMWARE_VERSION,
        (unsigned long)((uint64_t)esp_timer_get_time() / 1000000ULL),
        (unsigned long)mesh.tx_packets, (unsigned long)mesh.rx_packets,
        (unsigned long)mesh.rejected_commands,
        (unsigned long)observer.queued, (int)connectivity.wifi_rssi_dbm,
        location);
    if (written > 0 && written < (int)sizeof(payload)) {
        queue_payload("status", payload, true);
    }
    secure_zero(&settings, sizeof(settings));
}

static const char *route_code(uint8_t route)
{
    switch (route) {
    case D1L_MESHCORE_ROUTE_FLOOD: return "F";
    case D1L_MESHCORE_ROUTE_DIRECT: return "D";
    case D1L_MESHCORE_ROUTE_TRANSPORT_FLOOD: return "T";
    case D1L_MESHCORE_ROUTE_TRANSPORT_DIRECT: return "U";
    default: return "U";
    }
}

esp_err_t d1l_observer_enqueue_packet(const uint8_t *raw, size_t raw_len,
                                      int16_t rssi_dbm,
                                      int8_t snr_quarter_db)
{
    if (!raw || raw_len == 0U || raw_len > D1L_MESHCORE_MAX_RAW_PACKET) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    const bool enabled = s_status.enabled;
    portEXIT_CRITICAL(&s_lock);
    if (!enabled || s_public_key_hex[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_packet_queue_count == D1L_OBSERVER_PACKET_QUEUE_CAPACITY) {
        secure_zero(&s_packet_queue[s_packet_queue_head],
                    sizeof(s_packet_queue[s_packet_queue_head]));
        s_packet_queue_head =
            (s_packet_queue_head + 1U) % D1L_OBSERVER_PACKET_QUEUE_CAPACITY;
        s_packet_queue_count--;
        s_status.dropped_oldest++;
    }
    const size_t slot = (s_packet_queue_head + s_packet_queue_count) %
                        D1L_OBSERVER_PACKET_QUEUE_CAPACITY;
    memcpy(s_packet_queue[slot].raw, raw, raw_len);
    s_packet_queue[slot].raw_len = (uint8_t)raw_len;
    s_packet_queue[slot].rssi_dbm = rssi_dbm;
    s_packet_queue[slot].snr_quarter_db = snr_quarter_db;
    s_packet_queue_count++;
    s_status.queued =
        (uint32_t)(s_queue_count + s_packet_queue_count);
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

static bool build_next_packet_payload(void)
{
    d1l_observer_packet_entry_t entry = {0};
    portENTER_CRITICAL(&s_lock);
    const bool available = s_packet_queue_count > 0U;
    if (available) entry = s_packet_queue[s_packet_queue_head];
    portEXIT_CRITICAL(&s_lock);
    if (!available) return false;
    d1l_meshcore_wire_packet_t packet = {0};
    if (!d1l_meshcore_wire_decode_v1(entry.raw, entry.raw_len, &packet)) {
        portENTER_CRITICAL(&s_lock);
        secure_zero(&s_packet_queue[s_packet_queue_head],
                    sizeof(s_packet_queue[s_packet_queue_head]));
        s_packet_queue_head =
            (s_packet_queue_head + 1U) % D1L_OBSERVER_PACKET_QUEUE_CAPACITY;
        s_packet_queue_count--;
        s_status.queued =
            (uint32_t)(s_queue_count + s_packet_queue_count);
        portEXIT_CRITICAL(&s_lock);
        return true;
    }
    char timestamp[32] = {0};
    char clock[12] = {0};
    char date[12] = {0};
    if (!current_utc(NULL, timestamp, sizeof(timestamp), clock,
                     sizeof(clock), date, sizeof(date))) {
        return false;
    }
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    char origin[2U * D1L_NODE_NAME_LEN] = {0};
    if (!json_escape(settings.node_name, origin, sizeof(origin))) {
        snprintf(origin, sizeof(origin), "DeskOS D1L");
    }
    char raw_hex[D1L_MESHCORE_MAX_RAW_PACKET * 2U + 1U] = {0};
    bytes_to_hex(entry.raw, entry.raw_len, raw_hex, true);
    const int snr_hundredths = (int)entry.snr_quarter_db * 25;
    const bool snr_negative = snr_hundredths < 0;
    const int snr_abs = snr_negative ? -snr_hundredths : snr_hundredths;
    char payload[D1L_OBSERVER_PAYLOAD_LEN] = {0};
    const int written = snprintf(
        payload, sizeof(payload),
        "{\"origin\":\"%s\",\"origin_id\":\"%s\","
        "\"timestamp\":\"%s\",\"type\":\"PACKET\","
        "\"direction\":\"rx\",\"time\":\"%s\",\"date\":\"%s\","
        "\"len\":\"%u\",\"packet_type\":\"%u\",\"route\":\"%s\","
        "\"payload_len\":\"%u\",\"raw\":\"%s\","
        "\"SNR\":\"%s%d.%02d\",\"RSSI\":\"%d\"}",
        origin, s_public_key_hex, timestamp, clock, date,
        (unsigned)entry.raw_len, (unsigned)packet.type,
        route_code(packet.route), (unsigned)packet.payload_len, raw_hex,
        snr_negative ? "-" : "", snr_abs / 100, snr_abs % 100,
        (int)entry.rssi_dbm);
    secure_zero(&settings, sizeof(settings));
    if (written <= 0 || written >= (int)sizeof(payload)) {
        return false;
    }
    queue_payload("packets", payload, false);
    portENTER_CRITICAL(&s_lock);
    if (s_packet_queue_count > 0U &&
        s_packet_queue[s_packet_queue_head].raw_len == entry.raw_len &&
        memcmp(s_packet_queue[s_packet_queue_head].raw, entry.raw,
               entry.raw_len) == 0) {
        secure_zero(&s_packet_queue[s_packet_queue_head],
                    sizeof(s_packet_queue[s_packet_queue_head]));
        s_packet_queue_head =
            (s_packet_queue_head + 1U) % D1L_OBSERVER_PACKET_QUEUE_CAPACITY;
        s_packet_queue_count--;
        s_status.queued =
            (uint32_t)(s_queue_count + s_packet_queue_count);
    }
    portEXIT_CRITICAL(&s_lock);
    secure_zero(&entry, sizeof(entry));
    return true;
}

static void endpoint_topic(uint8_t index, const char *suffix,
                           char *out, size_t out_size)
{
    portENTER_CRITICAL(&s_lock);
    if (index == D1L_OBSERVER_CUSTOM_INDEX) {
        snprintf(out, out_size, "%s/%s", s_config.topic, suffix);
    } else {
        snprintf(out, out_size, "meshcore/%s/%s/%s", s_config.region,
                 s_public_key_hex, suffix);
    }
    portEXIT_CRITICAL(&s_lock);
}

static void process_endpoint_queue(uint8_t index)
{
    portENTER_CRITICAL(&s_lock);
    const bool inflight = s_endpoints[index].inflight_sequence != 0U;
    portEXIT_CRITICAL(&s_lock);
    if (inflight) return;
    d1l_observer_queue_entry_t entry = {0};
    if (!peek_payload_for_endpoint(index, &entry)) return;
    char topic[D1L_OBSERVER_TOPIC_LEN + 16U] = {0};
    endpoint_topic(index, entry.suffix, topic, sizeof(topic));
    const int message_id = publish_to_endpoint(
        index, topic, entry.payload, entry.retain);
    if (message_id >= 0) {
        portENTER_CRITICAL(&s_lock);
        s_endpoints[index].inflight_sequence = entry.sequence;
        s_endpoints[index].inflight_message_id = message_id;
        s_status.published_total++;
        s_status.last_message_id = (uint32_t)message_id;
        portEXIT_CRITICAL(&s_lock);
    }
    secure_zero(&entry, sizeof(entry));
}

static void observer_task(void *argument)
{
    (void)argument;
    for (;;) {
        const uint32_t now_ms =
            (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
        d1l_connectivity_status_t connectivity = {0};
        d1l_connectivity_status(&connectivity);
        const bool enabled = observer_enabled();
        if (!enabled || !connectivity.wifi_connected ||
            s_public_key_hex[0] == '\0') {
            stop_all_clients();
            refresh_aggregate_state(enabled, connectivity.wifi_connected);
            vTaskDelay(pdMS_TO_TICKS(D1L_OBSERVER_LOOP_MS));
            continue;
        }
        const esp_err_t time_ret = d1l_time_service_wait_for_network_time(
            D1L_TIME_TLS_WAIT_TIMEOUT_MS, D1L_TIME_TLS_WAIT_SLICE_MS,
            observer_network_continue, NULL);
        if (time_ret != ESP_OK) {
            stop_all_clients();
            note_endpoint_error(time_ret,
                                now_ms + D1L_OBSERVER_BACKOFF_MS);
            refresh_aggregate_state(enabled, connectivity.wifi_connected);
            vTaskDelay(pdMS_TO_TICKS(D1L_OBSERVER_LOOP_MS));
            continue;
        }
        (void)build_next_packet_payload();
        portENTER_CRITICAL(&s_lock);
        const uint8_t active_mask = active_endpoint_mask_locked();
        portEXIT_CRITICAL(&s_lock);
        uint32_t now_epoch = 0U;
        (void)current_utc(&now_epoch, NULL, 0U, NULL, 0U, NULL, 0U);
        bool started_endpoint = false;
        for (uint8_t i = 0U; i < D1L_OBSERVER_BROKER_COUNT; ++i) {
            if ((active_mask & (1U << i)) == 0U) {
                stop_endpoint(i);
                continue;
            }
            bool connected = false;
            uint32_t retry_at = 0U;
            uint32_t token_issued_at = 0U;
            portENTER_CRITICAL(&s_lock);
            connected = s_endpoints[i].connected;
            retry_at = s_endpoints[i].backoff_until_ms;
            token_issued_at = s_endpoints[i].token_issued_at;
            portEXIT_CRITICAL(&s_lock);
            if (token_issued_at > 0U &&
                now_epoch >= token_issued_at + D1L_OBSERVER_TOKEN_RENEW_SEC) {
                stop_endpoint(i);
                connected = false;
            }
            if (!endpoint_client_exists(i) &&
                (retry_at == 0U || (int32_t)(now_ms - retry_at) >= 0)) {
                bool another_connected = false;
                uint32_t last_start_ms = 0U;
                portENTER_CRITICAL(&s_lock);
                for (uint8_t j = 0U; j < D1L_OBSERVER_BROKER_COUNT; ++j) {
                    another_connected = another_connected ||
                                        s_endpoints[j].connected;
                }
                last_start_ms = s_last_endpoint_start_ms;
                portEXIT_CRITICAL(&s_lock);
                const bool start_window_open =
                    last_start_ms == 0U || another_connected ||
                    now_ms - last_start_ms >=
                        D1L_OBSERVER_ENDPOINT_START_GAP_MS;
                if (!started_endpoint && start_window_open) {
                    (void)start_endpoint(i, now_ms);
                    portENTER_CRITICAL(&s_lock);
                    s_last_endpoint_start_ms = now_ms;
                    portEXIT_CRITICAL(&s_lock);
                    started_endpoint = true;
                }
            }
            portENTER_CRITICAL(&s_lock);
            connected = s_endpoints[i].connected;
            portEXIT_CRITICAL(&s_lock);
            if (connected) process_endpoint_queue(i);
        }
        refresh_aggregate_state(enabled, connectivity.wifi_connected);
        d1l_observer_status_t status = {0};
        d1l_observer_status(&status);
        if (status.connected &&
            (s_last_periodic_ms == 0U ||
             now_ms - s_last_periodic_ms >=
                 D1L_OBSERVER_PUBLISH_INTERVAL_MS)) {
            enqueue_status_payload();
            s_last_periodic_ms = now_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(D1L_OBSERVER_LOOP_MS));
    }
}

esp_err_t d1l_observer_manager_init(void)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_OBSERVER_MQTT)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_task) return ESP_OK;
    if (!s_client_lock) {
        s_client_lock = xSemaphoreCreateMutex();
        if (!s_client_lock) return ESP_ERR_NO_MEM;
    }
    d1l_observer_config_t config = {0};
    const esp_err_t load_ret = load_config(&config);
    const esp_err_t identity_ret = refresh_public_identity();
    s_endpoints[0] = (d1l_observer_endpoint_t) {
        .index = 0U,
        .uri = D1L_OBSERVER_PRIMARY_URI,
        .audience = D1L_OBSERVER_PRIMARY_AUDIENCE,
    };
    s_endpoints[1] = (d1l_observer_endpoint_t) {
        .index = 1U,
        .uri = D1L_OBSERVER_SECONDARY_URI,
        .audience = D1L_OBSERVER_SECONDARY_AUDIENCE,
    };
    s_endpoints[2] = (d1l_observer_endpoint_t) {.index = 2U};
    portENTER_CRITICAL(&s_lock);
    s_config = config;
    s_endpoints[2].uri = s_config.configured ? s_config.uri : NULL;
    s_status.initialized = true;
    s_status.configured = identity_ret == ESP_OK;
    s_status.include_location = config.include_location;
    s_status.custom_configured = config.configured;
    snprintf(s_status.region, sizeof(s_status.region), "%s", config.region);
    snprintf(s_status.broker_host, sizeof(s_status.broker_host),
             "mqtt1.meshcore.ca + mqtt2.meshcore.ca");
    snprintf(s_status.topic, sizeof(s_status.topic), "meshcore/%s/%s",
             config.region, s_public_key_hex);
    s_status.last_error = load_ret != ESP_OK ? load_ret : identity_ret;
    portEXIT_CRITICAL(&s_lock);
    secure_zero(&config, sizeof(config));
    if (load_ret != ESP_OK || identity_ret != ESP_OK) {
        return load_ret != ESP_OK ? load_ret : identity_ret;
    }
    if (xTaskCreateWithCaps(observer_task, "d1l_observer",
                            D1L_OBSERVER_TASK_STACK_BYTES, NULL, 3, &s_task,
                            MALLOC_CAP_SPIRAM) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "observer", "ready",
                         "MeshCore Canada broker pair ready");
    return ESP_OK;
}

esp_err_t d1l_observer_configure(const char *mqtts_uri, const char *topic,
                                 const char *username, const char *password,
                                 bool include_location)
{
    if (!secure_mqtt_uri_valid(mqtts_uri) ||
        !text_valid(topic, D1L_OBSERVER_TOPIC_LEN, false) ||
        !text_valid(username, D1L_OBSERVER_USERNAME_LEN, true) ||
        !text_valid(password, D1L_OBSERVER_PASSWORD_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_observer_config_t config = {0};
    portENTER_CRITICAL(&s_lock);
    snprintf(config.region, sizeof(config.region), "%s", s_config.region);
    portEXIT_CRITICAL(&s_lock);
    config.include_location = include_location;
    config.configured = true;
    snprintf(config.uri, sizeof(config.uri), "%s", mqtts_uri);
    snprintf(config.topic, sizeof(config.topic), "%s", topic);
    snprintf(config.username, sizeof(config.username), "%s",
             username ? username : "");
    snprintf(config.password, sizeof(config.password), "%s",
             password ? password : "");
    if (!take_client_lock()) {
        secure_zero(&config, sizeof(config));
        return ESP_ERR_TIMEOUT;
    }
    stop_endpoint_locked(&s_endpoints[D1L_OBSERVER_CUSTOM_INDEX]);
    const esp_err_t ret = save_config(&config);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        secure_zero(&s_config, sizeof(s_config));
        s_config = config;
        s_endpoints[D1L_OBSERVER_CUSTOM_INDEX].uri = s_config.uri;
        s_status.custom_configured = true;
        s_status.include_location = include_location;
        portEXIT_CRITICAL(&s_lock);
    }
    give_client_lock();
    secure_zero(&config, sizeof(config));
    return ret;
}

esp_err_t d1l_observer_clear_configuration(void)
{
    if (!take_client_lock()) return ESP_ERR_TIMEOUT;
    stop_endpoint_locked(&s_endpoints[D1L_OBSERVER_CUSTOM_INDEX]);
    nvs_handle_t handle = 0U;
    esp_err_t ret = nvs_open(D1L_OBSERVER_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        const char *keys[] = {"uri", "topic", "user", "pass", "location"};
        for (size_t i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            const esp_err_t erase_ret = nvs_erase_key(handle, keys[i]);
            if (erase_ret != ESP_OK && erase_ret != ESP_ERR_NVS_NOT_FOUND) {
                ret = erase_ret;
                break;
            }
        }
        if (ret == ESP_OK) ret = nvs_commit(handle);
        nvs_close(handle);
    }
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        char region[D1L_OBSERVER_REGION_LEN] = {0};
        snprintf(region, sizeof(region), "%s", s_config.region);
        secure_zero(&s_config, sizeof(s_config));
        snprintf(s_config.region, sizeof(s_config.region), "%s", region);
        s_endpoints[D1L_OBSERVER_CUSTOM_INDEX].uri = NULL;
        s_status.custom_configured = false;
        s_status.custom_connected = false;
        s_status.include_location = false;
        for (size_t i = 0U; i < s_queue_count; ++i) {
            const size_t slot =
                (s_queue_head + i) % D1L_OBSERVER_QUEUE_CAPACITY;
            s_queue[slot].pending_mask &=
                (uint8_t)~(1U << D1L_OBSERVER_CUSTOM_INDEX);
        }
        prune_queue_locked();
        portEXIT_CRITICAL(&s_lock);
    }
    give_client_lock();
    return ret;
}

esp_err_t d1l_observer_set_enabled(bool enabled)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_OBSERVER_MQTT)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (enabled && s_public_key_hex[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_settings_t settings = {.observer_enabled = enabled};
    const esp_err_t ret = d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_OBSERVER);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.enabled = enabled;
        if (!enabled) {
            secure_zero(s_queue, sizeof(s_queue));
            secure_zero(s_packet_queue, sizeof(s_packet_queue));
            s_queue_head = 0U;
            s_queue_count = 0U;
            s_packet_queue_head = 0U;
            s_packet_queue_count = 0U;
            s_status.queued = 0U;
        }
        portEXIT_CRITICAL(&s_lock);
        d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "observer",
                             enabled ? "enabled" : "disabled",
                             enabled ? "packet uplink enabled" :
                                       "packet uplink disabled");
    }
    return ret;
}

esp_err_t d1l_observer_set_region(const char *iata)
{
    if (!region_valid(iata)) return ESP_ERR_INVALID_ARG;
    const esp_err_t ret = save_region(iata);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        snprintf(s_config.region, sizeof(s_config.region), "%s", iata);
        snprintf(s_status.region, sizeof(s_status.region), "%s", iata);
        snprintf(s_status.topic, sizeof(s_status.topic), "meshcore/%s/%s",
                 iata, s_public_key_hex);
        portEXIT_CRITICAL(&s_lock);
    }
    return ret;
}

void d1l_observer_status(d1l_observer_status_t *out_status)
{
    if (!out_status) return;
    portENTER_CRITICAL(&s_lock);
    *out_status = s_status;
    out_status->primary_diagnostic = s_endpoints[0].diagnostic;
    out_status->secondary_diagnostic = s_endpoints[1].diagnostic;
    out_status->custom_diagnostic = s_endpoints[2].diagnostic;
    out_status->primary_diagnostic.last_error = s_endpoints[0].last_error;
    out_status->secondary_diagnostic.last_error = s_endpoints[1].last_error;
    out_status->custom_diagnostic.last_error = s_endpoints[2].last_error;
    out_status->queued =
        (uint32_t)(s_queue_count + s_packet_queue_count);
    portEXIT_CRITICAL(&s_lock);
}

const char *d1l_observer_state_name(d1l_observer_state_t state)
{
    switch (state) {
    case D1L_OBSERVER_STATE_DISABLED: return "disabled";
    case D1L_OBSERVER_STATE_NOT_CONFIGURED: return "not_configured";
    case D1L_OBSERVER_STATE_WAITING_FOR_WIFI: return "waiting_for_wifi";
    case D1L_OBSERVER_STATE_CONNECTING: return "connecting";
    case D1L_OBSERVER_STATE_CONNECTED: return "connected";
    case D1L_OBSERVER_STATE_BACKOFF: return "backoff";
    case D1L_OBSERVER_STATE_ERROR: return "error";
    default: return "invalid";
    }
}
