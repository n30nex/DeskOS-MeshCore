#include "connectivity_manager.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"
#include "comms/ble_companion.h"
#include "comms/connectivity_boot_guard.h"
#include "comms/wifi_retry_policy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifdef CONFIG_ESP_WIFI_ENABLED
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs.h"
#endif

#define D1L_WIFI_RETRY_TASK_STACK_BYTES 3072U
#define D1L_WIFI_RETRY_TASK_PRIORITY 5U
#define D1L_WIFI_CONTROL_LOCK_TIMEOUT_MS 10000U
#define D1L_CONNECTIVITY_GUARD_LOCK_TIMEOUT_MS 10000U
#define D1L_CONNECTIVITY_GUARD_STABLE_WINDOW_MS 30000U
#define D1L_CONNECTIVITY_GUARD_NAMESPACE "d1l_conn"
#define D1L_CONNECTIVITY_GUARD_KEY "boot_guard"

static bool s_wifi_started;
static bool s_wifi_connected;
static bool s_wifi_connecting;
static char s_wifi_ip[16] = "";
static char s_wifi_last_error[32] = "none";
static uint16_t s_wifi_last_disconnect_reason;
static d1l_wifi_retry_policy_t s_wifi_policy;
static d1l_wifi_local_leave_token_t s_wifi_local_leave_token;
static portMUX_TYPE s_wifi_policy_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_connectivity_boot_guard_record_t s_boot_guard;
static d1l_connectivity_boot_guard_decision_t s_boot_guard_decision;
static bool s_boot_guard_ready;
static bool s_boot_guard_recovered;
static esp_err_t s_boot_guard_error = ESP_ERR_INVALID_STATE;

#ifdef CONFIG_ESP_WIFI_ENABLED
static bool s_wifi_initialized;
static esp_netif_t *s_wifi_sta_netif;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
static TaskHandle_t s_wifi_retry_task;
static SemaphoreHandle_t s_wifi_control_lock;
static SemaphoreHandle_t s_boot_guard_lock;
static bool s_boot_guard_stable_ack_pending;
static uint64_t s_boot_guard_stable_ack_due_ms;
static uint32_t s_boot_guard_stable_ack_record_generation;
#endif

static bool build_wifi_enabled(void)
{
#ifdef CONFIG_ESP_WIFI_ENABLED
    return true;
#else
    return false;
#endif
}

static bool build_ble_enabled(void)
{
    return d1l_ble_companion_build_enabled();
}

static bool release_wifi_user_control_available(void)
{
    return d1l_release_feature_available(
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL);
}

static bool release_ble_available(void)
{
    return d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE);
}

static d1l_wifi_retry_policy_t wifi_policy_snapshot(void)
{
    d1l_wifi_retry_policy_t snapshot;
    portENTER_CRITICAL(&s_wifi_policy_lock);
    snapshot = s_wifi_policy;
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    return snapshot;
}

static void set_wifi_last_error(const char *reason)
{
    snprintf(s_wifi_last_error, sizeof(s_wifi_last_error), "%s",
             reason ? reason : "unknown");
}

#ifdef CONFIG_ESP_WIFI_ENABLED
static bool reset_reason_is_crash_like(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
           reason == ESP_RST_BROWNOUT;
}

static void set_boot_guard_result(esp_err_t error, bool recovered)
{
    portENTER_CRITICAL(&s_wifi_policy_lock);
    s_boot_guard_ready = error == ESP_OK;
    s_boot_guard_recovered = recovered;
    s_boot_guard_error = error;
    portEXIT_CRITICAL(&s_wifi_policy_lock);
}

static void cancel_boot_guard_stable_ack(void);

static esp_err_t take_boot_guard_lock(void)
{
    if (!s_boot_guard_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t timeout_ticks =
        pdMS_TO_TICKS(D1L_CONNECTIVITY_GUARD_LOCK_TIMEOUT_MS);
    if (timeout_ticks == 0U) {
        timeout_ticks = 1U;
    }
    return xSemaphoreTake(s_boot_guard_lock, timeout_ticks) == pdTRUE ?
        ESP_OK : ESP_ERR_TIMEOUT;
}

static void give_boot_guard_lock(void)
{
    if (s_boot_guard_lock) {
        xSemaphoreGive(s_boot_guard_lock);
    }
}

static esp_err_t persist_boot_guard_record(
    const d1l_connectivity_boot_guard_record_t *record)
{
    if (!record || !d1l_connectivity_boot_guard_valid(record)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_CONNECTIVITY_GUARD_NAMESPACE,
                             NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_blob(handle, D1L_CONNECTIVITY_GUARD_KEY,
                       record, sizeof(*record));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t load_and_advance_boot_guard_unlocked(void)
{
    d1l_connectivity_boot_guard_record_t record = {0};
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_CONNECTIVITY_GUARD_NAMESPACE,
                             NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        set_boot_guard_result(ret, false);
        return ret;
    }

    size_t length = sizeof(record);
    ret = nvs_get_blob(handle, D1L_CONNECTIVITY_GUARD_KEY,
                       &record, &length);
    const bool missing = ret == ESP_ERR_NVS_NOT_FOUND;
    const bool valid = ret == ESP_OK && length == sizeof(record) &&
        d1l_connectivity_boot_guard_valid(&record);
    const bool recoverable = missing || ret == ESP_ERR_NVS_INVALID_LENGTH ||
        (ret == ESP_OK && !valid);
    if (!valid && !recoverable) {
        nvs_close(handle);
        set_boot_guard_result(ret, false);
        return ret;
    }
    if (!valid) {
        memset(&record, 0, sizeof(record));
    }

    const d1l_connectivity_boot_guard_decision_t decision =
        d1l_connectivity_boot_guard_note_boot(
            &record, reset_reason_is_crash_like(esp_reset_reason()));
    ret = nvs_set_blob(handle, D1L_CONNECTIVITY_GUARD_KEY,
                       &record, sizeof(record));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        set_boot_guard_result(ret, !missing && !valid);
        return ret;
    }

    portENTER_CRITICAL(&s_wifi_policy_lock);
    s_boot_guard = record;
    s_boot_guard_decision = decision;
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    set_boot_guard_result(ESP_OK, !missing && !valid);
    return ESP_OK;
}

static esp_err_t load_and_advance_boot_guard(void)
{
    const esp_err_t lock_ret = take_boot_guard_lock();
    if (lock_ret != ESP_OK) {
        set_boot_guard_result(lock_ret, false);
        return lock_ret;
    }
    const esp_err_t ret = load_and_advance_boot_guard_unlocked();
    give_boot_guard_lock();
    return ret;
}

static esp_err_t mark_boot_guard_active_unlocked(
    d1l_connectivity_subsystem_t subsystem)
{
    d1l_connectivity_boot_guard_record_t record;
    bool ready;
    bool recovered;
    portENTER_CRITICAL(&s_wifi_policy_lock);
    record = s_boot_guard;
    ready = s_boot_guard_ready;
    recovered = s_boot_guard_recovered;
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    if (ready && d1l_connectivity_boot_guard_valid(&record) &&
        record.last_active_subsystem == (uint8_t)subsystem) {
        return ESP_OK;
    }
    d1l_connectivity_boot_guard_mark_active(&record, subsystem);
    const esp_err_t ret = persist_boot_guard_record(&record);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_wifi_policy_lock);
        s_boot_guard = record;
        portEXIT_CRITICAL(&s_wifi_policy_lock);
    }
    set_boot_guard_result(ret, recovered);
    return ret;
}

static esp_err_t mark_boot_guard_active(
    d1l_connectivity_subsystem_t subsystem)
{
    const esp_err_t lock_ret = take_boot_guard_lock();
    if (lock_ret != ESP_OK) {
        set_boot_guard_result(lock_ret, false);
        return lock_ret;
    }
    const esp_err_t ret = mark_boot_guard_active_unlocked(subsystem);
    give_boot_guard_lock();
    return ret;
}

static esp_err_t clear_boot_guard_unlocked(void)
{
    d1l_connectivity_boot_guard_record_t record;
    bool ready;
    bool recovered;
    portENTER_CRITICAL(&s_wifi_policy_lock);
    record = s_boot_guard;
    ready = s_boot_guard_ready;
    recovered = s_boot_guard_recovered;
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    if (ready && d1l_connectivity_boot_guard_valid(&record) &&
        record.last_active_subsystem == D1L_CONNECTIVITY_SUBSYSTEM_NONE &&
        record.consecutive_crash_boots == 0U) {
        return ESP_OK;
    }
    d1l_connectivity_boot_guard_clear(&record);
    const esp_err_t ret = persist_boot_guard_record(&record);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_wifi_policy_lock);
        s_boot_guard = record;
        s_boot_guard_decision =
            (d1l_connectivity_boot_guard_decision_t){0};
        portEXIT_CRITICAL(&s_wifi_policy_lock);
    }
    set_boot_guard_result(ret, recovered);
    return ret;
}

static esp_err_t clear_boot_guard(void)
{
    cancel_boot_guard_stable_ack();
    const esp_err_t lock_ret = take_boot_guard_lock();
    if (lock_ret != ESP_OK) {
        set_boot_guard_result(lock_ret, false);
        return lock_ret;
    }
    const esp_err_t ret = clear_boot_guard_unlocked();
    give_boot_guard_lock();
    return ret;
}

static esp_err_t acknowledge_boot_guard_stable_unlocked(
    d1l_connectivity_subsystem_t subsystem,
    uint32_t expected_record_generation)
{
    d1l_connectivity_boot_guard_record_t record;
    bool ready;
    bool recovered;
    esp_err_t prior_error;
    portENTER_CRITICAL(&s_wifi_policy_lock);
    record = s_boot_guard;
    ready = s_boot_guard_ready;
    recovered = s_boot_guard_recovered;
    prior_error = s_boot_guard_error;
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    if (!ready || !d1l_connectivity_boot_guard_valid(&record)) {
        return prior_error == ESP_OK ? ESP_ERR_INVALID_STATE : prior_error;
    }
    if (record.generation != expected_record_generation) {
        return ESP_OK;
    }
    if (!d1l_connectivity_boot_guard_acknowledge_stable(
            &record, subsystem)) {
        return ESP_OK;
    }
    const esp_err_t ret = persist_boot_guard_record(&record);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_wifi_policy_lock);
        s_boot_guard = record;
        s_boot_guard_decision.crash_attributed = false;
        s_boot_guard_decision.crash_loop_detected = false;
        s_boot_guard_decision.consecutive_crash_boots = 0U;
        portEXIT_CRITICAL(&s_wifi_policy_lock);
    }
    set_boot_guard_result(ret, recovered);
    return ret;
}

static esp_err_t acknowledge_boot_guard_stable(
    d1l_connectivity_subsystem_t subsystem,
    uint32_t expected_record_generation)
{
    const esp_err_t lock_ret = take_boot_guard_lock();
    if (lock_ret != ESP_OK) {
        set_boot_guard_result(lock_ret, false);
        return lock_ret;
    }
    const esp_err_t ret = acknowledge_boot_guard_stable_unlocked(
        subsystem, expected_record_generation);
    give_boot_guard_lock();
    return ret;
}

static void cancel_boot_guard_stable_ack(void)
{
    portENTER_CRITICAL(&s_wifi_policy_lock);
    s_boot_guard_stable_ack_pending = false;
    s_boot_guard_stable_ack_due_ms = 0U;
    s_boot_guard_stable_ack_record_generation = 0U;
    portEXIT_CRITICAL(&s_wifi_policy_lock);
}

static bool take_boot_guard_stable_ack_if_due(
    uint64_t now_ms,
    uint64_t *out_due_ms,
    uint32_t *out_record_generation)
{
    if (!out_due_ms || !out_record_generation) {
        return false;
    }
    *out_due_ms = 0U;
    *out_record_generation = 0U;
    bool due = false;
    portENTER_CRITICAL(&s_wifi_policy_lock);
    if (s_boot_guard_stable_ack_pending &&
        s_wifi_policy.state != D1L_WIFI_RUNTIME_CONNECTED &&
        s_wifi_policy.state != D1L_WIFI_RUNTIME_SCANNING) {
        s_boot_guard_stable_ack_pending = false;
        s_boot_guard_stable_ack_due_ms = 0U;
        s_boot_guard_stable_ack_record_generation = 0U;
    }
    if (s_boot_guard_stable_ack_pending) {
        *out_due_ms = s_boot_guard_stable_ack_due_ms;
        *out_record_generation =
            s_boot_guard_stable_ack_record_generation;
        due = now_ms >= s_boot_guard_stable_ack_due_ms;
        if (due) {
            s_boot_guard_stable_ack_pending = false;
            s_boot_guard_stable_ack_due_ms = 0U;
            s_boot_guard_stable_ack_record_generation = 0U;
        }
    }
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    return due;
}

static uint64_t wifi_now_ms(void)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us <= 0 ? 0U : (uint64_t)now_us / 1000U;
}

static void notify_wifi_retry_worker(void)
{
    if (s_wifi_retry_task) {
        xTaskNotifyGive(s_wifi_retry_task);
    }
}

static void arm_local_leave_token(void)
{
    portENTER_CRITICAL(&s_wifi_policy_lock);
    d1l_wifi_local_leave_token_arm(&s_wifi_local_leave_token);
    portEXIT_CRITICAL(&s_wifi_policy_lock);
}

static void clear_local_leave_token(void)
{
    portENTER_CRITICAL(&s_wifi_policy_lock);
    d1l_wifi_local_leave_token_clear(&s_wifi_local_leave_token);
    portEXIT_CRITICAL(&s_wifi_policy_lock);
}

static bool consume_expected_local_leave(uint8_t reason)
{
    portENTER_CRITICAL(&s_wifi_policy_lock);
    const bool expected = d1l_wifi_local_leave_token_consume(
        &s_wifi_local_leave_token, reason == WIFI_REASON_ASSOC_LEAVE);
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    return expected;
}

static bool take_wifi_control(void)
{
    TickType_t timeout_ticks =
        pdMS_TO_TICKS(D1L_WIFI_CONTROL_LOCK_TIMEOUT_MS);
    if (timeout_ticks == 0U) {
        timeout_ticks = 1U;
    }
    return s_wifi_control_lock &&
           xSemaphoreTake(s_wifi_control_lock, timeout_ticks) == pdTRUE;
}

static void give_wifi_control(void)
{
    if (s_wifi_control_lock) {
        xSemaphoreGive(s_wifi_control_lock);
    }
}

static d1l_wifi_failure_class_t classify_disconnect_reason(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return D1L_WIFI_FAILURE_AUTH;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return D1L_WIFI_FAILURE_AP_UNAVAILABLE;
    default:
        return D1L_WIFI_FAILURE_TRANSIENT;
    }
}

static TickType_t retry_wait_ticks(uint64_t due_ms, uint64_t now_ms)
{
    if (due_ms <= now_ms) {
        return 0U;
    }
    const uint64_t delta_ms = due_ms - now_ms;
    const uint32_t bounded_ms = delta_ms > UINT32_MAX ? UINT32_MAX :
        (uint32_t)delta_ms;
    TickType_t ticks = pdMS_TO_TICKS(bounded_ms);
    if (ticks == 0U) {
        ticks = 1U;
    }
    return ticks;
}

static void wifi_retry_worker(void *context)
{
    (void)context;
    while (true) {
        const uint64_t now_ms = wifi_now_ms();
        uint64_t stable_ack_due_ms = 0U;
        uint32_t stable_ack_record_generation = 0U;
        if (take_boot_guard_stable_ack_if_due(
                now_ms, &stable_ack_due_ms,
                &stable_ack_record_generation)) {
            const esp_err_t guard_ret = acknowledge_boot_guard_stable(
                D1L_CONNECTIVITY_SUBSYSTEM_WIFI,
                stable_ack_record_generation);
            if (guard_ret != ESP_OK) {
                set_wifi_last_error("boot_guard_stable_ack_failed");
            }
            continue;
        }
        const d1l_wifi_retry_policy_t snapshot = wifi_policy_snapshot();
        if (!snapshot.retry_scheduled ||
            snapshot.state == D1L_WIFI_RUNTIME_SCANNING) {
            const TickType_t wait_ticks = stable_ack_due_ms > now_ms ?
                retry_wait_ticks(stable_ack_due_ms, now_ms) : portMAX_DELAY;
            (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
            continue;
        }

        const TickType_t wait_ticks = retry_wait_ticks(snapshot.retry_due_ms,
                                                       now_ms);
        if (wait_ticks > 0U) {
            (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
            continue;
        }

        if (!take_wifi_control()) {
            set_wifi_last_error("retry_control_failed");
            continue;
        }

        bool retry_due;
        const uint64_t retry_now_ms = wifi_now_ms();
        portENTER_CRITICAL(&s_wifi_policy_lock);
        retry_due = d1l_wifi_retry_policy_begin_due_retry(&s_wifi_policy,
                                                          retry_now_ms);
        portEXIT_CRITICAL(&s_wifi_policy_lock);
        if (!retry_due) {
            give_wifi_control();
            continue;
        }

        s_wifi_connecting = true;
        const esp_err_t ret = esp_wifi_connect();
        give_wifi_control();
        if (ret == ESP_OK) {
            set_wifi_last_error("none");
            continue;
        }

        s_wifi_connecting = false;
        set_wifi_last_error(esp_err_to_name(ret));
        const uint64_t failure_now_ms = wifi_now_ms();
        const uint32_t jitter_seed = esp_random();
        uint32_t delay_ms = 0U;
        portENTER_CRITICAL(&s_wifi_policy_lock);
        const bool scheduled = d1l_wifi_retry_policy_on_disconnect(
            &s_wifi_policy, D1L_WIFI_FAILURE_TRANSIENT, failure_now_ms,
            jitter_seed, &delay_ms);
        portEXIT_CRITICAL(&s_wifi_policy_lock);
        if (scheduled) {
            notify_wifi_retry_worker();
        }
    }
}

static esp_err_t ensure_wifi_retry_worker(void)
{
    if (s_wifi_retry_task) {
        return ESP_OK;
    }
    if (!s_wifi_control_lock) {
        s_wifi_control_lock = xSemaphoreCreateMutex();
        if (!s_wifi_control_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreate(wifi_retry_worker, "wifi_retry",
                    D1L_WIFI_RETRY_TASK_STACK_BYTES, NULL,
                    D1L_WIFI_RETRY_TASK_PRIORITY,
                    &s_wifi_retry_task) != pdPASS) {
        s_wifi_retry_task = NULL;
        vSemaphoreDelete(s_wifi_control_lock);
        s_wifi_control_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t ok_if_invalid_state(esp_err_t ret)
{
    return ret == ESP_ERR_INVALID_STATE ? ESP_OK : ret;
}

static const char *wifi_auth_name(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa_psk";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2_psk";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa_wpa2_psk";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "wpa2_enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3_psk";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2_wpa3_psk";
    case WIFI_AUTH_WAPI_PSK:
        return "wapi_psk";
    default:
        return "unknown";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        cancel_boot_guard_stable_ack();
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        const uint8_t reason = event ? event->reason : WIFI_REASON_UNSPECIFIED;
        const d1l_wifi_failure_class_t failure =
            classify_disconnect_reason(reason);
        const uint64_t now_ms = wifi_now_ms();
        const uint32_t jitter_seed = esp_random();
        s_wifi_connected = false;
        s_wifi_connecting = false;
        s_wifi_ip[0] = '\0';
        s_wifi_last_disconnect_reason = reason;
        const bool suppress_retry = consume_expected_local_leave(reason);
        if (suppress_retry) {
            set_wifi_last_error("none");
            return;
        }
        uint32_t delay_ms = 0U;
        portENTER_CRITICAL(&s_wifi_policy_lock);
        const bool scheduled = d1l_wifi_retry_policy_on_disconnect(
            &s_wifi_policy, failure, now_ms, jitter_seed, &delay_ms);
        const d1l_wifi_retry_policy_t snapshot = s_wifi_policy;
        portEXIT_CRITICAL(&s_wifi_policy_lock);
        if (snapshot.user_cancelled || !snapshot.enabled) {
            set_wifi_last_error(snapshot.user_cancelled ? "cancelled" : "none");
        } else if (snapshot.safe_mode) {
            set_wifi_last_error("safe_mode_disabled");
        } else {
            set_wifi_last_error(d1l_wifi_failure_class_name(failure));
        }
        if (scheduled) {
            notify_wifi_retry_worker();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        portENTER_CRITICAL(&s_wifi_policy_lock);
        d1l_wifi_retry_policy_mark_starting(&s_wifi_policy);
        portEXIT_CRITICAL(&s_wifi_policy_lock);
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_CONNECTED) {
        portENTER_CRITICAL(&s_wifi_policy_lock);
        d1l_wifi_retry_policy_mark_connecting(&s_wifi_policy);
        const bool connection_accepted =
            s_wifi_policy.state == D1L_WIFI_RUNTIME_CONNECTING;
        portEXIT_CRITICAL(&s_wifi_policy_lock);
        s_wifi_connecting = connection_accepted;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        const uint64_t connected_at_ms = wifi_now_ms();
        const uint64_t stable_due_ms =
            UINT64_MAX - connected_at_ms <
                    D1L_CONNECTIVITY_GUARD_STABLE_WINDOW_MS ?
                UINT64_MAX :
                connected_at_ms + D1L_CONNECTIVITY_GUARD_STABLE_WINDOW_MS;
        clear_local_leave_token();
        portENTER_CRITICAL(&s_wifi_policy_lock);
        d1l_wifi_retry_policy_connected(&s_wifi_policy);
        const bool connection_accepted =
            s_wifi_policy.state == D1L_WIFI_RUNTIME_CONNECTED;
        if (connection_accepted) {
            s_boot_guard_stable_ack_pending = true;
            s_boot_guard_stable_ack_due_ms = stable_due_ms;
            s_boot_guard_stable_ack_record_generation =
                s_boot_guard.generation;
        }
        portEXIT_CRITICAL(&s_wifi_policy_lock);
        if (!connection_accepted) {
            s_wifi_connected = false;
            s_wifi_connecting = false;
            s_wifi_ip[0] = '\0';
            return;
        }
        s_wifi_connected = true;
        s_wifi_connecting = false;
        s_wifi_last_disconnect_reason = 0U;
        if (event) {
            snprintf(s_wifi_ip, sizeof(s_wifi_ip), IPSTR, IP2STR(&event->ip_info.ip));
        }
        set_wifi_last_error("none");
        notify_wifi_retry_worker();
    }
}

static void stop_wifi_runtime(void)
{
    cancel_boot_guard_stable_ack();
    clear_local_leave_token();
    if (s_wifi_event_instance) {
        (void)esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_instance);
        s_wifi_event_instance = NULL;
    }
    if (s_ip_event_instance) {
        (void)esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_event_instance);
        s_ip_event_instance = NULL;
    }
    if (s_wifi_started) {
        (void)esp_wifi_disconnect();
    }
    if (s_wifi_initialized) {
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
    }
    if (s_wifi_sta_netif) {
        esp_netif_destroy_default_wifi(s_wifi_sta_netif);
        s_wifi_sta_netif = NULL;
    }
    s_wifi_started = false;
    s_wifi_initialized = false;
    s_wifi_connected = false;
    s_wifi_connecting = false;
    s_wifi_ip[0] = '\0';
}

static esp_err_t fail_closed_wifi_runtime(esp_err_t failure, const char *reason)
{
    char original_reason[sizeof(s_wifi_last_error)];
    snprintf(original_reason, sizeof(original_reason), "%s",
             reason ? reason : esp_err_to_name(failure));

    portENTER_CRITICAL(&s_wifi_policy_lock);
    d1l_wifi_retry_policy_disable(&s_wifi_policy);
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    notify_wifi_retry_worker();
    stop_wifi_runtime();

    d1l_settings_t safe_settings = {0};
    (void)d1l_settings_public_snapshot(&safe_settings);
    if (safe_settings.wifi_enabled) {
        safe_settings.wifi_enabled = false;
        esp_err_t rollback_ret = d1l_settings_update_fields(
            &safe_settings, D1L_SETTINGS_UPDATE_WIFI_ENABLED);
        if (rollback_ret != ESP_OK) {
            set_wifi_last_error("rollback_save_failed");
            return rollback_ret;
        }
    }

    const esp_err_t guard_ret = clear_boot_guard();
    if (guard_ret != ESP_OK) {
        set_wifi_last_error("boot_guard_clear_failed");
        return guard_ret;
    }

    set_wifi_last_error(original_reason);
    return failure;
}

static const char *wifi_boot_recovery_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
        return "boot_recovery_panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return "boot_recovery_wdt";
    case ESP_RST_BROWNOUT:
        return "boot_recovery_brownout";
    default:
        return NULL;
    }
}

static esp_err_t ensure_wifi_started(void)
{
    if (!build_wifi_enabled()) {
        set_wifi_last_error("build_disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t ret = ensure_wifi_retry_worker();
    if (ret != ESP_OK) {
        return fail_closed_wifi_runtime(ret, "retry_task_failed");
    }
    if (!s_wifi_initialized) {
        ret = ok_if_invalid_state(esp_netif_init());
        if (ret != ESP_OK) {
            return fail_closed_wifi_runtime(ret, esp_err_to_name(ret));
        }
        ret = ok_if_invalid_state(esp_event_loop_create_default());
        if (ret != ESP_OK) {
            return fail_closed_wifi_runtime(ret, esp_err_to_name(ret));
        }
        if (!s_wifi_sta_netif) {
            esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
            s_wifi_sta_netif = esp_netif_new(&netif_config);
            if (!s_wifi_sta_netif) {
                return fail_closed_wifi_runtime(ESP_FAIL, "sta_netif_failed");
            }
            ret = esp_netif_attach_wifi_station(s_wifi_sta_netif);
            if (ret != ESP_OK) {
                return fail_closed_wifi_runtime(ret, esp_err_to_name(ret));
            }
            ret = esp_wifi_set_default_wifi_sta_handlers();
            if (ret != ESP_OK) {
                return fail_closed_wifi_runtime(ret, esp_err_to_name(ret));
            }
        }
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ret = esp_wifi_init(&cfg);
        if (ret != ESP_OK) {
            return fail_closed_wifi_runtime(ret, esp_err_to_name(ret));
        }
        s_wifi_initialized = true;
        ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (ret != ESP_OK) {
            return fail_closed_wifi_runtime(ret, esp_err_to_name(ret));
        }
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (ret != ESP_OK) {
            return fail_closed_wifi_runtime(ret, esp_err_to_name(ret));
        }
    }
    if (!s_wifi_event_instance) {
        esp_err_t ret = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL,
            &s_wifi_event_instance);
        if (ret != ESP_OK) {
            return fail_closed_wifi_runtime(ret, esp_err_to_name(ret));
        }
    }
    if (!s_ip_event_instance) {
        esp_err_t ret = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL,
            &s_ip_event_instance);
        if (ret != ESP_OK) {
            return fail_closed_wifi_runtime(ret, esp_err_to_name(ret));
        }
    }
    if (!s_wifi_started) {
        esp_err_t ret = esp_wifi_start();
        if (ret != ESP_OK) {
            return fail_closed_wifi_runtime(ret, esp_err_to_name(ret));
        }
        s_wifi_started = true;
        set_wifi_last_error("none");
    }
    return ESP_OK;
}

static void copy_scan_record(d1l_wifi_scan_ap_t *dest, const wifi_ap_record_t *src)
{
    if (!dest || !src) {
        return;
    }
    memset(dest, 0, sizeof(*dest));
    size_t len = 0;
    while (len < sizeof(src->ssid) && src->ssid[len] != '\0') {
        len++;
    }
    if (len >= sizeof(dest->ssid)) {
        len = sizeof(dest->ssid) - 1U;
    }
    memcpy(dest->ssid, src->ssid, len);
    dest->ssid[len] = '\0';
    dest->rssi_dbm = src->rssi;
    dest->channel = src->primary;
    dest->auth = wifi_auth_name(src->authmode);
}

static void copy_wifi_config_field(uint8_t *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0U) {
        return;
    }
    memset(dest, 0, dest_size);
    if (!src) {
        return;
    }
    size_t len = 0;
    while (len < dest_size && src[len] != '\0') {
        dest[len] = (uint8_t)src[len];
        len++;
    }
}

static void wipe_wifi_config(wifi_config_t *config)
{
    if (!config) {
        return;
    }
    volatile uint8_t *bytes = (volatile uint8_t *)config;
    for (size_t i = 0; i < sizeof(*config); ++i) {
        bytes[i] = 0U;
    }
}

static void finish_wifi_scan_policy(void)
{
    portENTER_CRITICAL(&s_wifi_policy_lock);
    d1l_wifi_retry_policy_finish_scan(&s_wifi_policy, s_wifi_connected,
                                      s_wifi_connecting);
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    notify_wifi_retry_worker();
}
#endif

static void fill_status(d1l_connectivity_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    const bool wifi_available = release_wifi_user_control_available();
    const bool ble_available = release_ble_available();
    const bool observer_available = d1l_release_feature_available(
        D1L_RELEASE_FEATURE_OBSERVER_MQTT);
    d1l_ble_companion_status_t ble_status = {0};
    d1l_ble_companion_status(&ble_status);
    const d1l_wifi_retry_policy_t policy = wifi_policy_snapshot();
    d1l_connectivity_boot_guard_record_t boot_guard;
    d1l_connectivity_boot_guard_decision_t boot_decision;
    bool boot_guard_ready;
    bool boot_guard_recovered;
    esp_err_t boot_guard_error;
    portENTER_CRITICAL(&s_wifi_policy_lock);
    boot_guard = s_boot_guard;
    boot_decision = s_boot_guard_decision;
    boot_guard_ready = s_boot_guard_ready;
    boot_guard_recovered = s_boot_guard_recovered;
    boot_guard_error = s_boot_guard_error;
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    out_status->usb_console_ready = true;
    out_status->wifi_enabled_setting =
        wifi_available && settings.wifi_enabled;
    out_status->ble_companion_enabled_setting =
        ble_available && settings.ble_companion_enabled;
    out_status->observer_enabled_setting =
        observer_available && settings.observer_enabled;
    out_status->wifi_build_enabled =
        wifi_available && build_wifi_enabled();
    out_status->ble_build_enabled =
        ble_available && build_ble_enabled();
    out_status->wifi_stack_active = wifi_available && s_wifi_started;
    out_status->wifi_connected = wifi_available && s_wifi_connected;
    out_status->wifi_connecting = wifi_available && s_wifi_connecting;
    out_status->wifi_retry_scheduled =
        wifi_available && policy.retry_scheduled;
    out_status->wifi_user_cancelled =
        wifi_available && policy.user_cancelled;
    out_status->wifi_safe_mode = wifi_available && policy.safe_mode;
    out_status->wifi_boot_guard_ready =
        wifi_available && boot_guard_ready;
    out_status->wifi_boot_guard_recovered =
        wifi_available && boot_guard_recovered;
    out_status->wifi_crash_loop_detected =
        wifi_available && boot_decision.crash_loop_detected;
    out_status->ble_stack_active =
        ble_available && ble_status.stack_initialized;
    out_status->wifi_profile_saved =
        wifi_available && settings.wifi_profile_saved;
    out_status->wifi_password_saved =
        wifi_available && d1l_settings_wifi_password_saved();
    out_status->wifi_scan_supported = out_status->wifi_build_enabled;
    out_status->wifi_state = !wifi_available ?
        "unsupported_in_release_profile" :
        out_status->wifi_build_enabled ?
            d1l_wifi_runtime_state_name(policy.state) : "off";
    out_status->ble_state = !ble_available ?
        "unsupported_in_release_profile" :
        !settings.ble_companion_enabled ? "off" :
        (ble_status.state ? ble_status.state : "error");
    snprintf(out_status->wifi_ssid, sizeof(out_status->wifi_ssid), "%s",
             wifi_available ? settings.wifi_ssid : "");
    out_status->wifi_ip = wifi_available ? s_wifi_ip : "";
    out_status->wifi_retry_attempt = policy.retry_attempt;
    out_status->wifi_consecutive_crash_boots =
        boot_guard.consecutive_crash_boots;
    out_status->wifi_last_disconnect_reason = s_wifi_last_disconnect_reason;
    out_status->wifi_retry_delay_ms = policy.retry_delay_ms;
    out_status->wifi_last_error = wifi_available ?
        s_wifi_last_error : "unsupported_in_release_profile";
    out_status->wifi_last_failure_class =
        d1l_wifi_failure_class_name(policy.last_failure);
    const d1l_connectivity_subsystem_t reported_subsystem =
        boot_decision.crash_attributed ?
            boot_decision.previous_active_subsystem :
            (d1l_connectivity_subsystem_t)boot_guard.last_active_subsystem;
    out_status->wifi_last_active_subsystem =
        d1l_connectivity_subsystem_name(reported_subsystem);
    out_status->wifi_boot_guard_error = esp_err_to_name(boot_guard_error);
#ifdef CONFIG_ESP_WIFI_ENABLED
    if (s_wifi_retry_task) {
        out_status->wifi_retry_task_stack_high_water_bytes =
            (uint32_t)uxTaskGetStackHighWaterMark(s_wifi_retry_task);
    }
    if (s_wifi_started && s_wifi_connected) {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out_status->wifi_rssi_dbm = ap.rssi;
            out_status->wifi_channel = ap.primary;
        }
    }
#endif
    out_status->coexistence_policy = "offline_first_one_companion_radio";
}

esp_err_t d1l_connectivity_prepare_reboot(void)
{
    const esp_err_t ble_ret = d1l_ble_companion_prepare_reboot();
    portENTER_CRITICAL(&s_wifi_policy_lock);
    d1l_wifi_retry_policy_cancel(&s_wifi_policy);
    portEXIT_CRITICAL(&s_wifi_policy_lock);
#ifdef CONFIG_ESP_WIFI_ENABLED
    notify_wifi_retry_worker();
    /* esp_restart() normally replays the default Wi-Fi shutdown handler. The
     * controlled ROM system-reset path bypasses shutdown handlers, so mirror
     * that handler without changing the persisted Wi-Fi setting or profile. */
    if (s_wifi_initialized) {
        if (s_wifi_control_lock && !take_wifi_control()) {
            return ble_ret == ESP_OK ? ESP_FAIL : ble_ret;
        }
        esp_err_t ret = esp_wifi_stop();
        if (s_wifi_control_lock) {
            give_wifi_control();
        }
        if (ret != ESP_OK) {
            return ble_ret == ESP_OK ? ret : ble_ret;
        }
    }
#endif
    s_wifi_started = false;
    s_wifi_connected = false;
    s_wifi_connecting = false;
    s_wifi_ip[0] = '\0';
    return ble_ret;
}

esp_err_t d1l_connectivity_init(void)
{
    if (!release_wifi_user_control_available()) {
        portENTER_CRITICAL(&s_wifi_policy_lock);
        d1l_wifi_retry_policy_init(&s_wifi_policy, false, false);
        portEXIT_CRITICAL(&s_wifi_policy_lock);
        set_wifi_last_error("unsupported_in_release_profile");
        return ESP_OK;
    }

    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    if (settings.wifi_enabled && settings.ble_companion_enabled) {
        settings.ble_companion_enabled = false;
        esp_err_t ret = d1l_settings_update_fields(
            &settings, D1L_SETTINGS_UPDATE_BLE_ENABLED);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    (void)d1l_settings_public_snapshot(&settings);
#ifdef CONFIG_ESP_WIFI_ENABLED
    if (!s_boot_guard_lock) {
        s_boot_guard_lock = xSemaphoreCreateMutex();
    }
    const esp_err_t boot_guard_ret = s_boot_guard_lock ?
        load_and_advance_boot_guard() : ESP_ERR_NO_MEM;
    if (!s_boot_guard_lock) {
        set_boot_guard_result(boot_guard_ret, false);
    }
#endif
    portENTER_CRITICAL(&s_wifi_policy_lock);
    d1l_wifi_retry_policy_init(
        &s_wifi_policy,
        settings.wifi_enabled && build_wifi_enabled(),
        settings.wifi_profile_saved && settings.wifi_ssid[0] != '\0');
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    if (settings.wifi_enabled && !build_wifi_enabled()) {
        set_wifi_last_error("build_disabled");
        return ESP_OK;
    }
    if (settings.ble_companion_enabled) {
        if (!build_ble_enabled()) {
            return ESP_OK;
        }
        const esp_err_t ble_ret = d1l_ble_companion_start();
        if (ble_ret != ESP_OK) {
            d1l_settings_t safe_settings = {0};
            safe_settings.ble_companion_enabled = false;
            (void)d1l_settings_update_fields(
                &safe_settings, D1L_SETTINGS_UPDATE_BLE_ENABLED);
            return ble_ret;
        }
    }
#ifdef CONFIG_ESP_WIFI_ENABLED
    if (settings.wifi_enabled && settings.wifi_profile_saved) {
        const char *recovery_reason = wifi_boot_recovery_reason();
        d1l_connectivity_boot_guard_decision_t decision;
        portENTER_CRITICAL(&s_wifi_policy_lock);
        decision = s_boot_guard_decision;
        portEXIT_CRITICAL(&s_wifi_policy_lock);
        const bool repeated_wifi_crash = decision.crash_loop_detected &&
            decision.previous_active_subsystem ==
                D1L_CONNECTIVITY_SUBSYSTEM_WIFI;
        /* A crash-like first boot after installing this schema has no prior
         * marker to trust. Contain that one boot, but do not arm Wi-Fi until
         * an explicit retry actually starts the subsystem. */
        const bool conservative_first_recovery = recovery_reason &&
            !decision.previous_record_valid;
        if (boot_guard_ret != ESP_OK || repeated_wifi_crash ||
            conservative_first_recovery) {
            portENTER_CRITICAL(&s_wifi_policy_lock);
            d1l_wifi_retry_policy_enter_safe_mode(&s_wifi_policy);
            portEXIT_CRITICAL(&s_wifi_policy_lock);
            if (boot_guard_ret != ESP_OK) {
                set_wifi_last_error("boot_guard_unavailable");
            } else if (decision.crash_loop_detected) {
                set_wifi_last_error("boot_recovery_wifi_crash_loop");
            } else {
                set_wifi_last_error(recovery_reason ? recovery_reason :
                                    "boot_recovery_wifi_crash");
            }
            return ESP_OK;
        }
        return d1l_connectivity_wifi_connect();
    }
    if (boot_guard_ret == ESP_OK) {
        const esp_err_t clear_ret = clear_boot_guard();
        if (clear_ret != ESP_OK) {
            set_wifi_last_error("boot_guard_clear_failed");
            return clear_ret;
        }
    }
#endif
    return ESP_OK;
}

void d1l_connectivity_status(d1l_connectivity_status_t *out_status)
{
    fill_status(out_status);
}

esp_err_t d1l_connectivity_wifi_scan(d1l_wifi_scan_result_t *out_result)
{
    if (!release_wifi_user_control_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->scan_supported = build_wifi_enabled();
    out_result->reason = "not_started";
    out_result->last_error = ESP_OK;
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    if (!settings.wifi_enabled) {
        out_result->reason = "disabled_by_setting";
        out_result->last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    if (!build_wifi_enabled()) {
        out_result->reason = "build_disabled";
        out_result->last_error = ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_NOT_SUPPORTED;
    }
#ifndef CONFIG_ESP_WIFI_ENABLED
    out_result->reason = "build_disabled";
    out_result->last_error = ESP_ERR_NOT_SUPPORTED;
    return ESP_ERR_NOT_SUPPORTED;
#else
    portENTER_CRITICAL(&s_wifi_policy_lock);
    const bool scan_eligible =
        d1l_wifi_retry_policy_scan_allowed(&s_wifi_policy);
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    if (!scan_eligible) {
        out_result->reason = "scan_cancelled";
        out_result->last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = mark_boot_guard_active(
        D1L_CONNECTIVITY_SUBSYSTEM_WIFI);
    if (ret != ESP_OK) {
        out_result->reason = "boot_guard_unavailable";
        out_result->last_error = ret;
        return ret;
    }
    ret = ensure_wifi_started();
    if (ret != ESP_OK) {
        out_result->reason = s_wifi_last_error;
        out_result->last_error = ret;
        return ret;
    }
    portENTER_CRITICAL(&s_wifi_policy_lock);
    const bool scan_started =
        d1l_wifi_retry_policy_begin_scan(&s_wifi_policy);
    const d1l_wifi_retry_policy_t begin_policy = s_wifi_policy;
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    if (!scan_started) {
        if (begin_policy.user_cancelled || !begin_policy.enabled) {
            const esp_err_t guard_ret = clear_boot_guard();
            if (guard_ret != ESP_OK) {
                out_result->reason = "boot_guard_clear_failed";
                out_result->last_error = guard_ret;
                return guard_ret;
            }
        }
        out_result->reason = "scan_cancelled";
        out_result->last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    if (!take_wifi_control()) {
        finish_wifi_scan_policy();
        out_result->reason = "scan_control_failed";
        out_result->last_error = ESP_FAIL;
        return ESP_FAIL;
    }
    const d1l_wifi_retry_policy_t scan_policy = wifi_policy_snapshot();
    if (scan_policy.state != D1L_WIFI_RUNTIME_SCANNING) {
        give_wifi_control();
        if (scan_policy.user_cancelled || !scan_policy.enabled) {
            const esp_err_t guard_ret = clear_boot_guard();
            if (guard_ret != ESP_OK) {
                out_result->reason = "boot_guard_clear_failed";
                out_result->last_error = guard_ret;
                return guard_ret;
            }
        }
        out_result->reason = "scan_cancelled";
        out_result->last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        give_wifi_control();
        finish_wifi_scan_policy();
        out_result->reason = esp_err_to_name(ret);
        out_result->last_error = ret;
        set_wifi_last_error(out_result->reason);
        return ret;
    }
    out_result->scan_started = true;

    uint16_t ap_count = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        give_wifi_control();
        finish_wifi_scan_policy();
        out_result->reason = esp_err_to_name(ret);
        out_result->last_error = ret;
        set_wifi_last_error(out_result->reason);
        return ret;
    }
    out_result->total_count = ap_count;
    uint16_t record_count = ap_count > D1L_WIFI_SCAN_MAX ? D1L_WIFI_SCAN_MAX : ap_count;
    out_result->truncated = ap_count > D1L_WIFI_SCAN_MAX;
    if (record_count > 0U) {
        wifi_ap_record_t records[D1L_WIFI_SCAN_MAX] = {0};
        uint16_t records_to_fetch = record_count;
        ret = esp_wifi_scan_get_ap_records(&records_to_fetch, records);
        if (ret != ESP_OK) {
            give_wifi_control();
            finish_wifi_scan_policy();
            out_result->reason = esp_err_to_name(ret);
            out_result->last_error = ret;
            set_wifi_last_error(out_result->reason);
            return ret;
        }
        out_result->returned_count = records_to_fetch;
        for (uint16_t i = 0; i < records_to_fetch; ++i) {
            copy_scan_record(&out_result->aps[i], &records[i]);
        }
    } else {
        out_result->returned_count = 0;
    }
    out_result->reason = "ok";
    out_result->last_error = ESP_OK;
    give_wifi_control();
    finish_wifi_scan_policy();
    set_wifi_last_error("none");
    return ESP_OK;
#endif
}

esp_err_t d1l_connectivity_wifi_connect(void)
{
    if (!release_wifi_user_control_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    if (!settings.wifi_enabled) {
        set_wifi_last_error("disabled_by_setting");
        return ESP_ERR_INVALID_STATE;
    }
    if (!settings.wifi_profile_saved || settings.wifi_ssid[0] == '\0') {
        set_wifi_last_error("profile_required");
        return ESP_ERR_INVALID_STATE;
    }
    if (!build_wifi_enabled()) {
        set_wifi_last_error("build_disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }
#ifdef CONFIG_ESP_WIFI_ENABLED
    cancel_boot_guard_stable_ack();
#endif
    portENTER_CRITICAL(&s_wifi_policy_lock);
    const bool connect_allowed =
        d1l_wifi_retry_policy_begin_manual_connect(&s_wifi_policy);
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    if (!connect_allowed) {
        set_wifi_last_error("profile_required");
        return ESP_ERR_INVALID_STATE;
    }
#ifndef CONFIG_ESP_WIFI_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    d1l_settings_wifi_secret_t wifi_secret = {0};
    esp_err_t ret = d1l_settings_wifi_secret_snapshot(&wifi_secret);
    if (ret != ESP_OK || !wifi_secret.wifi_enabled ||
        !wifi_secret.wifi_profile_saved || wifi_secret.wifi_ssid[0] == '\0') {
        d1l_settings_wifi_secret_wipe(&wifi_secret);
        set_wifi_last_error(ret == ESP_OK ? "profile_required" :
                                            esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    wifi_config_t config = {0};
    copy_wifi_config_field(config.sta.ssid, sizeof(config.sta.ssid),
                           wifi_secret.wifi_ssid);
    copy_wifi_config_field(config.sta.password, sizeof(config.sta.password),
                           wifi_secret.wifi_password);
    d1l_settings_wifi_secret_wipe(&wifi_secret);
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    ret = mark_boot_guard_active(
        D1L_CONNECTIVITY_SUBSYSTEM_WIFI);
    if (ret != ESP_OK) {
        wipe_wifi_config(&config);
        portENTER_CRITICAL(&s_wifi_policy_lock);
        d1l_wifi_retry_policy_enter_safe_mode(&s_wifi_policy);
        portEXIT_CRITICAL(&s_wifi_policy_lock);
        set_wifi_last_error("boot_guard_unavailable");
        return ret;
    }
    notify_wifi_retry_worker();
    ret = ensure_wifi_started();
    if (ret != ESP_OK) {
        wipe_wifi_config(&config);
        return ret;
    }
    if (!take_wifi_control()) {
        wipe_wifi_config(&config);
        set_wifi_last_error("connect_control_failed");
        return ESP_FAIL;
    }
    if (s_wifi_connected || s_wifi_connecting) {
        arm_local_leave_token();
        ret = esp_wifi_disconnect();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
            clear_local_leave_token();
            wipe_wifi_config(&config);
            give_wifi_control();
            return fail_closed_wifi_runtime(ret, esp_err_to_name(ret));
        }
        if (ret == ESP_ERR_WIFI_NOT_CONNECT) {
            clear_local_leave_token();
        }
        s_wifi_connected = false;
        s_wifi_connecting = false;
        s_wifi_ip[0] = '\0';
    }
    ret = esp_wifi_set_config(WIFI_IF_STA, &config);
    wipe_wifi_config(&config);
    if (ret != ESP_OK) {
        clear_local_leave_token();
        give_wifi_control();
        return fail_closed_wifi_runtime(ret, esp_err_to_name(ret));
    }
    ret = esp_wifi_connect();
    /* Retain one pending local-leave token across a successful asynchronous
     * connect.  Only an ASSOC_LEAVE event may consume it as intentional;
     * GOT_IP clears it, and every other disconnect clears then takes the
     * genuine-failure path below. */
    if (ret != ESP_OK) {
        clear_local_leave_token();
        s_wifi_connecting = false;
        set_wifi_last_error(esp_err_to_name(ret));
        const uint64_t now_ms = wifi_now_ms();
        const uint32_t jitter_seed = esp_random();
        uint32_t delay_ms = 0U;
        portENTER_CRITICAL(&s_wifi_policy_lock);
        const bool scheduled = d1l_wifi_retry_policy_on_disconnect(
            &s_wifi_policy, D1L_WIFI_FAILURE_TRANSIENT, now_ms,
            jitter_seed, &delay_ms);
        portEXIT_CRITICAL(&s_wifi_policy_lock);
        give_wifi_control();
        if (scheduled) {
            notify_wifi_retry_worker();
        }
        return ret;
    }
    portENTER_CRITICAL(&s_wifi_policy_lock);
    d1l_wifi_retry_policy_mark_connecting(&s_wifi_policy);
    const bool still_connecting =
        s_wifi_policy.state == D1L_WIFI_RUNTIME_CONNECTING;
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    if (!still_connecting) {
        (void)esp_wifi_disconnect();
        s_wifi_connected = false;
        s_wifi_connecting = false;
        s_wifi_ip[0] = '\0';
        give_wifi_control();
        const esp_err_t guard_ret = clear_boot_guard();
        if (guard_ret != ESP_OK) {
            set_wifi_last_error("boot_guard_clear_failed");
            return guard_ret;
        }
        set_wifi_last_error("cancelled");
        return ESP_ERR_INVALID_STATE;
    }
    s_wifi_connecting = true;
    give_wifi_control();
    set_wifi_last_error("none");
    return ESP_OK;
#endif
}

esp_err_t d1l_connectivity_wifi_disconnect(void)
{
    if (!release_wifi_user_control_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    portENTER_CRITICAL(&s_wifi_policy_lock);
    d1l_wifi_retry_policy_cancel(&s_wifi_policy);
    portEXIT_CRITICAL(&s_wifi_policy_lock);
#ifdef CONFIG_ESP_WIFI_ENABLED
    notify_wifi_retry_worker();
    if (s_wifi_started) {
        if (!take_wifi_control()) {
            set_wifi_last_error("disconnect_control_failed");
            return ESP_FAIL;
        }
        esp_err_t ret = esp_wifi_disconnect();
        give_wifi_control();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
            set_wifi_last_error(esp_err_to_name(ret));
            return ret;
        }
    }
#endif
    s_wifi_connected = false;
    s_wifi_connecting = false;
    s_wifi_ip[0] = '\0';
#ifdef CONFIG_ESP_WIFI_ENABLED
    const esp_err_t boot_guard_ret = clear_boot_guard();
    if (boot_guard_ret != ESP_OK) {
        set_wifi_last_error("boot_guard_clear_failed");
        return boot_guard_ret;
    }
#endif
    set_wifi_last_error("cancelled");
    return ESP_OK;
}

esp_err_t d1l_connectivity_set_wifi_enabled(bool enabled)
{
    if (!release_wifi_user_control_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (enabled && !build_wifi_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    d1l_settings_t settings = {0};
    esp_err_t ret;
    if (!enabled) {
        settings.wifi_enabled = false;
        ret = d1l_settings_update_fields(
            &settings, D1L_SETTINGS_UPDATE_WIFI_ENABLED);
        if (ret != ESP_OK) {
            return ret;
        }
        portENTER_CRITICAL(&s_wifi_policy_lock);
        d1l_wifi_retry_policy_disable(&s_wifi_policy);
        portEXIT_CRITICAL(&s_wifi_policy_lock);
#ifdef CONFIG_ESP_WIFI_ENABLED
        notify_wifi_retry_worker();
        if (s_wifi_control_lock && !take_wifi_control()) {
            set_wifi_last_error("disable_control_failed");
            return ESP_FAIL;
        }
        stop_wifi_runtime();
        if (s_wifi_control_lock) {
            give_wifi_control();
        }
        const esp_err_t boot_guard_ret = clear_boot_guard();
#else
        const esp_err_t boot_guard_ret = ESP_OK;
#endif
        if (boot_guard_ret != ESP_OK) {
            set_wifi_last_error("boot_guard_clear_failed");
            return boot_guard_ret;
        }
        set_wifi_last_error("none");
        return ESP_OK;
    }

    settings.ble_companion_enabled = false;
    ret = d1l_ble_companion_stop();
    if (ret != ESP_OK) {
        return ret;
    }
#ifdef CONFIG_ESP_WIFI_ENABLED
    ret = mark_boot_guard_active(D1L_CONNECTIVITY_SUBSYSTEM_WIFI);
    if (ret != ESP_OK) {
        set_wifi_last_error("boot_guard_unavailable");
        return ret;
    }
    ret = ensure_wifi_started();
    if (ret != ESP_OK) {
        return ret;
    }
#endif

    settings.wifi_enabled = true;
    ret = d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_WIFI_ENABLED |
                   D1L_SETTINGS_UPDATE_BLE_ENABLED);
    if (ret != ESP_OK) {
#ifdef CONFIG_ESP_WIFI_ENABLED
        stop_wifi_runtime();
        const esp_err_t guard_ret = clear_boot_guard();
        if (guard_ret != ESP_OK) {
            set_wifi_last_error("boot_guard_clear_failed");
            return guard_ret;
        }
#endif
        set_wifi_last_error(esp_err_to_name(ret));
        return ret;
    }

    (void)d1l_settings_public_snapshot(&settings);
    portENTER_CRITICAL(&s_wifi_policy_lock);
    d1l_wifi_retry_policy_configure(
        &s_wifi_policy, true,
        settings.wifi_profile_saved && settings.wifi_ssid[0] != '\0');
    portEXIT_CRITICAL(&s_wifi_policy_lock);

#ifdef CONFIG_ESP_WIFI_ENABLED
    if (settings.wifi_profile_saved) {
        ret = d1l_connectivity_wifi_connect();
        if (ret != ESP_OK) {
            return ret;
        }
    }
#endif
    return ESP_OK;
}

esp_err_t d1l_connectivity_set_ble_enabled(bool enabled)
{
    if (!release_ble_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (enabled && !build_ble_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    d1l_settings_t settings = {0};
    settings.ble_companion_enabled = enabled;
    if (enabled) {
        settings.wifi_enabled = false;
    }
    const d1l_settings_update_mask_t update_mask =
        D1L_SETTINGS_UPDATE_BLE_ENABLED |
        (enabled ? D1L_SETTINGS_UPDATE_WIFI_ENABLED : 0U);
    const esp_err_t ret = d1l_settings_update_fields(&settings, update_mask);
    if (ret != ESP_OK) {
        return ret;
    }
    if (enabled) {
        portENTER_CRITICAL(&s_wifi_policy_lock);
        d1l_wifi_retry_policy_disable(&s_wifi_policy);
        portEXIT_CRITICAL(&s_wifi_policy_lock);
#ifdef CONFIG_ESP_WIFI_ENABLED
        notify_wifi_retry_worker();
        if (s_wifi_control_lock && !take_wifi_control()) {
            return ESP_FAIL;
        }
        stop_wifi_runtime();
        if (s_wifi_control_lock) {
            give_wifi_control();
        }
#endif
    }
#ifdef CONFIG_ESP_WIFI_ENABLED
    /* This guard currently records Wi-Fi crash recovery only. Clear that
     * Wi-Fi-specific marker before starting the independently fail-closed BLE
     * foundation. */
    esp_err_t boot_guard_ret = ESP_OK;
    if (enabled) {
        boot_guard_ret = clear_boot_guard();
    }
#else
    const esp_err_t boot_guard_ret = ESP_OK;
#endif
    if (boot_guard_ret != ESP_OK) {
        set_wifi_last_error("boot_guard_unavailable");
        return boot_guard_ret;
    }
    if (!enabled) {
        return d1l_ble_companion_stop();
    }
    const esp_err_t start_ret = d1l_ble_companion_start();
    if (start_ret != ESP_OK) {
        d1l_settings_t safe_settings = {0};
        safe_settings.ble_companion_enabled = false;
        const esp_err_t rollback_ret = d1l_settings_update_fields(
            &safe_settings, D1L_SETTINGS_UPDATE_BLE_ENABLED);
        return rollback_ret == ESP_OK ? start_ret : rollback_ret;
    }
    return ESP_OK;
}

esp_err_t d1l_connectivity_save_wifi_profile(const char *ssid, const char *password)
{
    if (!release_wifi_user_control_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t ret = d1l_settings_save_wifi_profile(ssid, password);
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    portENTER_CRITICAL(&s_wifi_policy_lock);
    d1l_wifi_retry_policy_configure(
        &s_wifi_policy, settings.wifi_enabled && build_wifi_enabled(),
        settings.wifi_profile_saved && settings.wifi_ssid[0] != '\0');
    portEXIT_CRITICAL(&s_wifi_policy_lock);
    if (settings.wifi_enabled && build_wifi_enabled()) {
        return d1l_connectivity_wifi_connect();
    }
    return ESP_OK;
}

esp_err_t d1l_connectivity_clear_wifi_profile(void)
{
    if (!release_wifi_user_control_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t ret = d1l_settings_clear_wifi_profile();
    if (ret == ESP_OK) {
        const esp_err_t disconnect_ret =
            d1l_connectivity_wifi_disconnect();
        d1l_settings_t settings = {0};
        (void)d1l_settings_public_snapshot(&settings);
        portENTER_CRITICAL(&s_wifi_policy_lock);
        d1l_wifi_retry_policy_configure(
            &s_wifi_policy, settings.wifi_enabled && build_wifi_enabled(),
            false);
        portEXIT_CRITICAL(&s_wifi_policy_lock);
        if (disconnect_ret != ESP_OK) {
            return disconnect_ret;
        }
        set_wifi_last_error(settings.wifi_enabled ? "profile_required" : "none");
    }
    return ret;
}
