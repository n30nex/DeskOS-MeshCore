#include "observer_manager.h"

#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"
#include "app/settings_model.h"
#include "comms/connectivity_manager.h"
#include "diagnostics/event_log.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"

#include "mesh/meshcore_service.h"

#define D1L_OBSERVER_NAMESPACE "d1l_observer"
#define D1L_OBSERVER_QUEUE_CAPACITY 8U
#define D1L_OBSERVER_PAYLOAD_LEN 384U
#define D1L_OBSERVER_TASK_STACK_BYTES 6144U
#define D1L_OBSERVER_PUBLISH_INTERVAL_MS 60000U
#define D1L_OBSERVER_BACKOFF_MS 5000U
#define D1L_OBSERVER_LOOP_MS 200U

typedef struct {
    char uri[D1L_OBSERVER_URI_LEN];
    char topic[D1L_OBSERVER_TOPIC_LEN];
    char username[D1L_OBSERVER_USERNAME_LEN];
    char password[D1L_OBSERVER_PASSWORD_LEN];
    bool include_location;
    bool configured;
} d1l_observer_config_t;

typedef struct {
    char payload[D1L_OBSERVER_PAYLOAD_LEN];
} d1l_observer_queue_entry_t;

static const char *TAG = "d1l_observer";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_observer_config_t s_config;
static d1l_observer_queue_entry_t s_queue[D1L_OBSERVER_QUEUE_CAPACITY];
static size_t s_queue_head;
static size_t s_queue_count;
static d1l_observer_status_t s_status = {
    .state = D1L_OBSERVER_STATE_DISABLED,
    .queue_capacity = D1L_OBSERVER_QUEUE_CAPACITY,
};
static esp_mqtt_client_handle_t s_client;
static SemaphoreHandle_t s_client_lock;
static TaskHandle_t s_task;
static uint32_t s_last_periodic_ms;
static uint32_t s_backoff_until_ms;

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
        if (ch < 0x20U || ch == 0x7FU) {
            return false;
        }
    }
    return true;
}

static bool mqtts_uri_valid(const char *uri)
{
    return text_valid(uri, D1L_OBSERVER_URI_LEN, false) &&
           strncmp(uri, "mqtts://", sizeof("mqtts://") - 1U) == 0 &&
           uri[sizeof("mqtts://") - 1U] != '\0';
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

static esp_err_t load_config(d1l_observer_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    secure_zero(out, sizeof(*out));
    nvs_handle_t handle = 0U;
    esp_err_t ret = nvs_open(D1L_OBSERVER_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    size_t uri_size = sizeof(out->uri);
    size_t topic_size = sizeof(out->topic);
    size_t username_size = sizeof(out->username);
    size_t password_size = sizeof(out->password);
    uint8_t include_location = 0U;
    ret = nvs_get_str(handle, "uri", out->uri, &uri_size);
    if (ret == ESP_OK) {
        ret = nvs_get_str(handle, "topic", out->topic, &topic_size);
    }
    if (ret == ESP_OK) {
        const esp_err_t user_ret =
            nvs_get_str(handle, "user", out->username, &username_size);
        if (user_ret != ESP_OK && user_ret != ESP_ERR_NVS_NOT_FOUND) {
            ret = user_ret;
        }
    }
    if (ret == ESP_OK) {
        const esp_err_t pass_ret =
            nvs_get_str(handle, "pass", out->password, &password_size);
        if (pass_ret != ESP_OK && pass_ret != ESP_ERR_NVS_NOT_FOUND) {
            ret = pass_ret;
        }
    }
    if (ret == ESP_OK) {
        const esp_err_t location_ret =
            nvs_get_u8(handle, "location", &include_location);
        if (location_ret != ESP_OK &&
            location_ret != ESP_ERR_NVS_NOT_FOUND) {
            ret = location_ret;
        }
    }
    nvs_close(handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        secure_zero(out, sizeof(*out));
        return ESP_OK;
    }
    if (ret != ESP_OK || !mqtts_uri_valid(out->uri) ||
        !text_valid(out->topic, sizeof(out->topic), false) ||
        !text_valid(out->username, sizeof(out->username), true) ||
        !text_valid(out->password, sizeof(out->password), true)) {
        secure_zero(out, sizeof(*out));
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    out->include_location = include_location != 0U;
    out->configured = true;
    return ESP_OK;
}

static esp_err_t save_config(const d1l_observer_config_t *config)
{
    if (!config || !config->configured) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0U;
    esp_err_t ret = nvs_open(D1L_OBSERVER_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "uri", config->uri);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "topic", config->topic);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "user", config->username);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "pass", config->password);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, "location",
                         config->include_location ? 1U : 0U);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    return ret;
}

static void set_state(d1l_observer_state_t state, esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.connected = state == D1L_OBSERVER_STATE_CONNECTED;
    s_status.last_error = error;
    portEXIT_CRITICAL(&s_lock);
}

static void set_backoff_until(uint32_t deadline_ms)
{
    portENTER_CRITICAL(&s_lock);
    s_backoff_until_ms = deadline_ms;
    portEXIT_CRITICAL(&s_lock);
}

static uint32_t backoff_until(void)
{
    portENTER_CRITICAL(&s_lock);
    const uint32_t deadline_ms = s_backoff_until_ms;
    portEXIT_CRITICAL(&s_lock);
    return deadline_ms;
}

static void queue_payload(const char *payload)
{
    if (!payload || payload[0] == '\0') {
        return;
    }
    const size_t payload_length =
        strnlen(payload, D1L_OBSERVER_PAYLOAD_LEN - 1U);
    portENTER_CRITICAL(&s_lock);
    const size_t slot =
        (s_queue_head + s_queue_count) % D1L_OBSERVER_QUEUE_CAPACITY;
    if (s_queue_count == D1L_OBSERVER_QUEUE_CAPACITY) {
        s_queue_head = (s_queue_head + 1U) % D1L_OBSERVER_QUEUE_CAPACITY;
        s_status.dropped_oldest++;
    } else {
        s_queue_count++;
    }
    memcpy(s_queue[slot].payload, payload, payload_length);
    s_queue[slot].payload[payload_length] = '\0';
    s_status.queued = (uint32_t)s_queue_count;
    s_status.queued_total++;
    portEXIT_CRITICAL(&s_lock);
}

static bool peek_payload(char *out, size_t out_size)
{
    if (!out || out_size == 0U) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    const bool available = s_queue_count > 0U;
    if (available) {
        const size_t payload_length =
            strnlen(s_queue[s_queue_head].payload, out_size - 1U);
        memcpy(out, s_queue[s_queue_head].payload, payload_length);
        out[payload_length] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);
    return available;
}

static void pop_payload(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_queue_count > 0U) {
        secure_zero(&s_queue[s_queue_head], sizeof(s_queue[s_queue_head]));
        s_queue_head = (s_queue_head + 1U) % D1L_OBSERVER_QUEUE_CAPACITY;
        s_queue_count--;
        s_status.queued = (uint32_t)s_queue_count;
        s_status.published_total++;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void enqueue_status_payload(void)
{
    d1l_meshcore_service_status_t mesh = d1l_meshcore_service_status();
    d1l_connectivity_status_t connectivity = {0};
    d1l_connectivity_status(&connectivity);
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    char payload[D1L_OBSERVER_PAYLOAD_LEN] = {0};
    bool include_location = false;
    portENTER_CRITICAL(&s_lock);
    include_location = s_config.include_location;
    portEXIT_CRITICAL(&s_lock);
    if (include_location && settings.map_location_set) {
        snprintf(
            payload, sizeof(payload),
            "{\"schema\":1,\"event\":\"status\",\"uptime_ms\":%lu,"
            "\"mesh\":\"%s\",\"rx_packets\":%lu,\"tx_packets\":%lu,"
            "\"wifi_rssi_dbm\":%d,\"location\":{\"source\":\"manual_or_companion\","
            "\"lat_e7\":%ld,\"lon_e7\":%ld}}",
            (unsigned long)((uint64_t)esp_timer_get_time() / 1000ULL),
            d1l_meshcore_service_state_name(mesh.state),
            (unsigned long)mesh.rx_packets, (unsigned long)mesh.tx_packets,
            (int)connectivity.wifi_rssi_dbm, (long)settings.map_lat_e7,
            (long)settings.map_lon_e7);
    } else {
        snprintf(
            payload, sizeof(payload),
            "{\"schema\":1,\"event\":\"status\",\"uptime_ms\":%lu,"
            "\"mesh\":\"%s\",\"rx_packets\":%lu,\"tx_packets\":%lu,"
            "\"wifi_rssi_dbm\":%d}",
            (unsigned long)((uint64_t)esp_timer_get_time() / 1000ULL),
            d1l_meshcore_service_state_name(mesh.state),
            (unsigned long)mesh.rx_packets, (unsigned long)mesh.tx_packets,
            (int)connectivity.wifi_rssi_dbm);
    }
    secure_zero(&settings, sizeof(settings));
    queue_payload(payload);
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;
    const esp_mqtt_event_handle_t event =
        (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        portENTER_CRITICAL(&s_lock);
        s_status.state = D1L_OBSERVER_STATE_CONNECTED;
        s_status.connected = true;
        s_status.reconnects++;
        s_status.last_error = ESP_OK;
        s_backoff_until_ms = 0U;
        portEXIT_CRITICAL(&s_lock);
        d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "observer",
                             "connected", "TLS broker connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        set_state(D1L_OBSERVER_STATE_BACKOFF, ESP_ERR_INVALID_STATE);
        set_backoff_until(
            (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL) +
            D1L_OBSERVER_BACKOFF_MS);
        d1l_event_log_append(D1L_EVENT_LOG_LEVEL_WARN, "observer",
                             "disconnected", "broker connection lost");
        break;
    case MQTT_EVENT_PUBLISHED:
        portENTER_CRITICAL(&s_lock);
        s_status.acknowledged_total++;
        s_status.last_message_id =
            event && event->msg_id > 0 ? (uint32_t)event->msg_id : 0U;
        portEXIT_CRITICAL(&s_lock);
        break;
    case MQTT_EVENT_ERROR:
        set_state(D1L_OBSERVER_STATE_ERROR, ESP_FAIL);
        d1l_event_log_append(D1L_EVENT_LOG_LEVEL_ERROR, "observer",
                             "error", "TLS or MQTT transport error");
        break;
    default:
        break;
    }
}

static void stop_client_locked(void)
{
    if (!s_client) {
        return;
    }
    (void)esp_mqtt_client_stop(s_client);
    (void)esp_mqtt_client_destroy(s_client);
    s_client = NULL;
}

static esp_err_t start_client(void)
{
    if (!take_client_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    d1l_observer_config_t local = {0};
    portENTER_CRITICAL(&s_lock);
    local = s_config;
    portEXIT_CRITICAL(&s_lock);
    if (s_client || !local.configured) {
        const esp_err_t ret =
            s_client ? ESP_OK : ESP_ERR_INVALID_STATE;
        secure_zero(&local, sizeof(local));
        give_client_lock();
        return ret;
    }
    const esp_mqtt_client_config_t config = {
        .broker = {
            .address.uri = local.uri,
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
        },
        .credentials = {
            .username = local.username[0] ? local.username : NULL,
            .authentication.password =
                local.password[0] ? local.password : NULL,
        },
        .session = {
            .keepalive = 60,
            .disable_clean_session = false,
        },
        .network = {
            .reconnect_timeout_ms = D1L_OBSERVER_BACKOFF_MS,
            .timeout_ms = 10000,
            .disable_auto_reconnect = false,
        },
        .task = {
            .priority = 3,
            .stack_size = D1L_OBSERVER_TASK_STACK_BYTES,
        },
        .buffer = {
            .size = 2048,
            .out_size = 2048,
        },
        .outbox.limit = 8192,
    };
    s_client = esp_mqtt_client_init(&config);
    if (!s_client) {
        secure_zero(&local, sizeof(local));
        give_client_lock();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (ret == ESP_OK) {
        set_state(D1L_OBSERVER_STATE_CONNECTING, ESP_OK);
        ret = esp_mqtt_client_start(s_client);
    }
    if (ret != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        secure_zero(&local, sizeof(local));
        give_client_lock();
        return ret;
    }
    secure_zero(&local, sizeof(local));
    give_client_lock();
    return ESP_OK;
}

static void stop_client(void)
{
    if (!take_client_lock()) {
        return;
    }
    stop_client_locked();
    give_client_lock();
}

static bool client_exists(void)
{
    if (!take_client_lock()) {
        return true;
    }
    const bool exists = s_client != NULL;
    give_client_lock();
    return exists;
}

static int publish_payload(const char *topic, const char *payload)
{
    if (!topic || !payload || !take_client_lock()) {
        return -1;
    }
    const int message_id = s_client ?
        esp_mqtt_client_enqueue(
            s_client, topic, payload, 0, 1, 0, true) : -1;
    give_client_lock();
    return message_id;
}

static bool configured_and_enabled(void)
{
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    const bool enabled = settings.observer_enabled &&
        d1l_release_feature_available(D1L_RELEASE_FEATURE_OBSERVER_MQTT);
    portENTER_CRITICAL(&s_lock);
    const bool configured = s_config.configured;
    s_status.enabled = enabled;
    s_status.configured = configured;
    portEXIT_CRITICAL(&s_lock);
    return enabled && configured;
}

static void observer_task(void *argument)
{
    (void)argument;
    for (;;) {
        const uint32_t now_ms = (uint32_t)(
            (uint64_t)esp_timer_get_time() / 1000ULL);
        d1l_connectivity_status_t connectivity = {0};
        d1l_connectivity_status(&connectivity);
        if (!configured_and_enabled()) {
            stop_client();
            portENTER_CRITICAL(&s_lock);
            const bool configured = s_config.configured;
            portEXIT_CRITICAL(&s_lock);
            set_state(configured ? D1L_OBSERVER_STATE_DISABLED :
                                   D1L_OBSERVER_STATE_NOT_CONFIGURED,
                      ESP_OK);
        } else if (!connectivity.wifi_connected) {
            stop_client();
            set_state(D1L_OBSERVER_STATE_WAITING_FOR_WIFI, ESP_OK);
        } else {
            const uint32_t retry_at_ms = backoff_until();
            if (!client_exists() &&
                (retry_at_ms == 0U ||
                 (int32_t)(now_ms - retry_at_ms) >= 0)) {
                const esp_err_t ret = start_client();
                if (ret != ESP_OK) {
                    set_state(D1L_OBSERVER_STATE_BACKOFF, ret);
                    set_backoff_until(
                        now_ms + D1L_OBSERVER_BACKOFF_MS);
                }
            }
            d1l_observer_status_t status = {0};
            d1l_observer_status(&status);
            if (status.connected &&
                (s_last_periodic_ms == 0U ||
                 now_ms - s_last_periodic_ms >=
                     D1L_OBSERVER_PUBLISH_INTERVAL_MS)) {
                enqueue_status_payload();
                s_last_periodic_ms = now_ms;
            }
            char payload[D1L_OBSERVER_PAYLOAD_LEN] = {0};
            if (status.connected && peek_payload(payload, sizeof(payload))) {
                char topic[D1L_OBSERVER_TOPIC_LEN + 16U] = {0};
                char base_topic[D1L_OBSERVER_TOPIC_LEN] = {0};
                portENTER_CRITICAL(&s_lock);
                memcpy(base_topic, s_config.topic, sizeof(base_topic));
                portEXIT_CRITICAL(&s_lock);
                base_topic[sizeof(base_topic) - 1U] = '\0';
                snprintf(topic, sizeof(topic), "%s/status", base_topic);
                const int message_id = publish_payload(topic, payload);
                if (message_id >= 0) {
                    portENTER_CRITICAL(&s_lock);
                    s_status.last_message_id = (uint32_t)message_id;
                    portEXIT_CRITICAL(&s_lock);
                    pop_payload();
                } else if (message_id == -2) {
                    set_state(D1L_OBSERVER_STATE_BACKOFF, ESP_ERR_NO_MEM);
                } else {
                    set_state(D1L_OBSERVER_STATE_ERROR, ESP_FAIL);
                }
            }
            secure_zero(payload, sizeof(payload));
        }
        vTaskDelay(pdMS_TO_TICKS(D1L_OBSERVER_LOOP_MS));
    }
}

esp_err_t d1l_observer_manager_init(void)
{
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_OBSERVER_MQTT)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_task) {
        return ESP_OK;
    }
    if (!s_client_lock) {
        s_client_lock = xSemaphoreCreateMutex();
        if (!s_client_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    d1l_observer_config_t config = {0};
    const esp_err_t load_ret = load_config(&config);
    const bool configuration_loaded = config.configured;
    char broker_host[D1L_OBSERVER_URI_LEN] = {0};
    public_broker_name(config.uri, broker_host, sizeof(broker_host));
    portENTER_CRITICAL(&s_lock);
    s_config = config;
    s_status.initialized = true;
    s_status.configured = config.configured;
    s_status.include_location = config.include_location;
    memcpy(s_status.broker_host, broker_host,
           sizeof(s_status.broker_host));
    memcpy(s_status.topic, config.topic, sizeof(s_status.topic));
    s_status.topic[sizeof(s_status.topic) - 1U] = '\0';
    s_status.last_error = load_ret;
    portEXIT_CRITICAL(&s_lock);
    secure_zero(&config, sizeof(config));
    if (load_ret != ESP_OK) {
        set_state(D1L_OBSERVER_STATE_ERROR, load_ret);
        return load_ret;
    }
    if (xTaskCreate(observer_task, "d1l_observer",
                    D1L_OBSERVER_TASK_STACK_BYTES, NULL, 3, &s_task) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "observer", "ready",
                         configuration_loaded ? "configuration loaded" :
                                                "configuration required");
    return ESP_OK;
}

esp_err_t d1l_observer_configure(const char *mqtts_uri,
                                 const char *topic,
                                 const char *username,
                                 const char *password,
                                 bool include_location)
{
    if (!mqtts_uri_valid(mqtts_uri) ||
        !text_valid(topic, D1L_OBSERVER_TOPIC_LEN, false) ||
        !text_valid(username, D1L_OBSERVER_USERNAME_LEN, true) ||
        !text_valid(password, D1L_OBSERVER_PASSWORD_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_observer_config_t config = {
        .include_location = include_location,
        .configured = true,
    };
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
    const esp_err_t ret = save_config(&config);
    if (ret == ESP_OK) {
        stop_client_locked();
        char broker_host[D1L_OBSERVER_URI_LEN] = {0};
        public_broker_name(config.uri, broker_host, sizeof(broker_host));
        portENTER_CRITICAL(&s_lock);
        secure_zero(&s_config, sizeof(s_config));
        s_config = config;
        s_status.configured = true;
        s_status.include_location = include_location;
        memcpy(s_status.broker_host, broker_host,
               sizeof(s_status.broker_host));
        memcpy(s_status.topic, config.topic, sizeof(s_status.topic));
        s_status.topic[sizeof(s_status.topic) - 1U] = '\0';
        s_status.last_error = ESP_OK;
        s_backoff_until_ms = 0U;
        portEXIT_CRITICAL(&s_lock);
        d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "observer",
                             "configured", "TLS broker settings saved");
    }
    give_client_lock();
    secure_zero(&config, sizeof(config));
    return ret;
}

esp_err_t d1l_observer_clear_configuration(void)
{
    if (!take_client_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    stop_client_locked();
    nvs_handle_t handle = 0U;
    esp_err_t ret = nvs_open(D1L_OBSERVER_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        ret = nvs_erase_all(handle);
        if (ret == ESP_OK) {
            ret = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (ret == ESP_OK) {
        d1l_settings_t settings = {.observer_enabled = false};
        ret = d1l_settings_update_fields(
            &settings, D1L_SETTINGS_UPDATE_OBSERVER);
    }
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        secure_zero(&s_config, sizeof(s_config));
        secure_zero(s_queue, sizeof(s_queue));
        s_queue_head = 0U;
        s_queue_count = 0U;
        s_status.configured = false;
        s_status.enabled = false;
        s_status.connected = false;
        s_status.include_location = false;
        s_status.broker_host[0] = '\0';
        s_status.topic[0] = '\0';
        s_status.queued = 0U;
        s_status.state = D1L_OBSERVER_STATE_NOT_CONFIGURED;
        s_status.last_error = ESP_OK;
        s_backoff_until_ms = 0U;
        portEXIT_CRITICAL(&s_lock);
        d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "observer",
                             "cleared", "broker settings removed");
    }
    give_client_lock();
    return ret;
}

esp_err_t d1l_observer_set_enabled(bool enabled)
{
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_OBSERVER_MQTT)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    portENTER_CRITICAL(&s_lock);
    const bool configured = s_config.configured;
    portEXIT_CRITICAL(&s_lock);
    if (enabled && !configured) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_settings_t settings = {.observer_enabled = enabled};
    const esp_err_t ret = d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_OBSERVER);
    if (ret == ESP_OK) {
        d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "observer",
                             enabled ? "enabled" : "disabled",
                             enabled ? "explicit upload opt-in enabled" :
                                       "uploads disabled");
    }
    return ret;
}

void d1l_observer_status(d1l_observer_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out_status = s_status;
    out_status->queued = (uint32_t)s_queue_count;
    portEXIT_CRITICAL(&s_lock);
}

const char *d1l_observer_state_name(d1l_observer_state_t state)
{
    switch (state) {
    case D1L_OBSERVER_STATE_DISABLED:
        return "disabled";
    case D1L_OBSERVER_STATE_NOT_CONFIGURED:
        return "not_configured";
    case D1L_OBSERVER_STATE_WAITING_FOR_WIFI:
        return "waiting_for_wifi";
    case D1L_OBSERVER_STATE_CONNECTING:
        return "connecting";
    case D1L_OBSERVER_STATE_CONNECTED:
        return "connected";
    case D1L_OBSERVER_STATE_BACKOFF:
        return "backoff";
    case D1L_OBSERVER_STATE_ERROR:
        return "error";
    default:
        return "invalid";
    }
}
