#include "rp2040_bridge.h"

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hal/indicator_pins.h"
#include "hal/rp2040_file_reply.h"
#include "hal/rp2040_sd_reply.h"
#include "tca9535.h"

#define D1L_RP2040_UART_BUF_SIZE 4096
#define D1L_RP2040_PING_QUERY "DESKOS_SD_PING\n"
#define D1L_RP2040_PING_REPLY_PREFIX "DESKOS_SD_PING"
#define D1L_RP2040_BOOTLOADER_QUERY "DESKOS_SD_BOOTLOADER\n"
#define D1L_RP2040_BOOTLOADER_REPLY_PREFIX "DESKOS_SD_BOOTLOADER"
#define D1L_RP2040_SD_QUERY "DESKOS_SD_STATUS\n"
#define D1L_RP2040_SD_REPLY_PREFIX "DESKOS_SD_STATUS"
#define D1L_RP2040_SD_MOUNT_QUERY "DESKOS_SD_MOUNT\n"
#define D1L_RP2040_SD_MOUNT_REPLY_PREFIX "DESKOS_SD_MOUNT"
#define D1L_RP2040_SD_DIAG_QUERY "DESKOS_SD_DIAG\n"
#define D1L_RP2040_SD_DIAG_REPLY_PREFIX "DESKOS_SD_DIAG"
#define D1L_RP2040_FILE_PREFIX "DESKOS_SD_FILE"
#define D1L_RP2040_LINE_BUFFER_SIZE (D1L_RP2040_FILE_LINE_MAX + 1U)
#define D1L_RP2040_SD_DIAG_LINE_BUFFER_SIZE (D1L_RP2040_SD_DIAG_LINE_MAX + 1U)
#define D1L_RP2040_PATH64_MAX (((D1L_RP2040_FILE_PATH_MAX + 2U) / 3U) * 4U)
#define D1L_RP2040_DATA64_MAX (((D1L_RP2040_FILE_CHUNK_MAX + 2U) / 3U) * 4U)
#define D1L_RP2040_BRIDGE_LOCK_GRACE_MS 15000U

static d1l_rp2040_status_t s_status = {
    .uart_ready = false,
    .init_result = ESP_ERR_INVALID_STATE,
    .buffered_bytes = 0,
};

static uint16_t s_file_request_id = 1;
static StaticSemaphore_t s_bridge_mutex_storage;
static SemaphoreHandle_t s_bridge_mutex;
static TaskHandle_t s_bridge_quiesce_owner;
static portMUX_TYPE s_bridge_quiesce_owner_lock = portMUX_INITIALIZER_UNLOCKED;

static void ensure_bridge_mutex(void)
{
    if (s_bridge_mutex == NULL) {
        s_bridge_mutex = xSemaphoreCreateMutexStatic(&s_bridge_mutex_storage);
    }
}

static esp_err_t take_bridge_lock(uint32_t timeout_ms)
{
    ensure_bridge_mutex();
    if (s_bridge_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const uint32_t base_ms = timeout_ms > 0 ? timeout_ms : 500U;
    const uint32_t wait_ms = base_ms > (UINT32_MAX - D1L_RP2040_BRIDGE_LOCK_GRACE_MS) ?
                                 UINT32_MAX :
                                 base_ms + D1L_RP2040_BRIDGE_LOCK_GRACE_MS;
    TickType_t ticks = pdMS_TO_TICKS(wait_ms);
    if (ticks == 0) {
        ticks = 1;
    }
    return xSemaphoreTake(s_bridge_mutex, ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void give_bridge_lock(void)
{
    if (s_bridge_mutex != NULL) {
        xSemaphoreGive(s_bridge_mutex);
    }
}

static TickType_t bridge_quiesce_remaining_ticks(int64_t started_us,
                                                  uint32_t timeout_ms)
{
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    const int64_t timeout_us = (int64_t)timeout_ms * 1000LL;
    if (elapsed_us >= timeout_us) {
        return 0;
    }
    const uint32_t remaining_ms =
        (uint32_t)((timeout_us - elapsed_us + 999LL) / 1000LL);
    TickType_t ticks = pdMS_TO_TICKS(remaining_ms);
    return ticks == 0U ? 1U : ticks;
}

esp_err_t d1l_rp2040_bridge_quiesce_begin(uint32_t timeout_ms)
{
    if (timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t started_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_bridge_quiesce_owner_lock);
    const bool already_quiesced = s_bridge_quiesce_owner != NULL;
    portEXIT_CRITICAL(&s_bridge_quiesce_owner_lock);
    if (already_quiesced) {
        return ESP_ERR_INVALID_STATE;
    }
    ensure_bridge_mutex();
    if (!s_bridge_mutex) {
        return ESP_ERR_NO_MEM;
    }
    TickType_t ticks = bridge_quiesce_remaining_ticks(started_us, timeout_ms);
    if (ticks == 0U || xSemaphoreTake(s_bridge_mutex, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* esp_restart_noos waits for every enabled UART without an application
     * timeout. Prove TX idle while this lock excludes all new bridge writes. */
    const uart_port_t uart_port = (uart_port_t)s_status.uart_port;
    if (uart_is_driver_installed(uart_port)) {
        ticks = bridge_quiesce_remaining_ticks(started_us, timeout_ms);
        if (ticks == 0U) {
            give_bridge_lock();
            return ESP_ERR_TIMEOUT;
        }
        const esp_err_t tx_idle_ret = uart_wait_tx_done(uart_port, ticks);
        if (tx_idle_ret != ESP_OK) {
            give_bridge_lock();
            return tx_idle_ret;
        }
    }
    if (bridge_quiesce_remaining_ticks(started_us, timeout_ms) == 0U) {
        give_bridge_lock();
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_bridge_quiesce_owner_lock);
    s_bridge_quiesce_owner = xTaskGetCurrentTaskHandle();
    portEXIT_CRITICAL(&s_bridge_quiesce_owner_lock);
    return ESP_OK;
}

void d1l_rp2040_bridge_quiesce_end(void)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_bridge_quiesce_owner_lock);
    if (s_bridge_quiesce_owner != current) {
        portEXIT_CRITICAL(&s_bridge_quiesce_owner_lock);
        return;
    }
    s_bridge_quiesce_owner = NULL;
    portEXIT_CRITICAL(&s_bridge_quiesce_owner_lock);
    give_bridge_lock();
}

static esp_err_t pulse_rp2040_reset(const d1l_rp2040_pins_t *pins, uint32_t hold_ms)
{
    const uint32_t hold = hold_ms > 0 ? hold_ms : 100U;
    esp_err_t ret = tca9535_set_level(pins->expander_reset, false);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(hold));
    return tca9535_set_level(pins->expander_reset, true);
}

static bool line_has_prefix(const char *line, const char *prefix)
{
    const size_t prefix_len = strlen(prefix);
    return line &&
           strncmp(line, prefix, prefix_len) == 0 &&
           (line[prefix_len] == '\0' || line[prefix_len] == ' ');
}

static bool token_value_span(const char *line, const char *key, const char **out_value,
                             size_t *out_len)
{
    if (!line || !key || !out_value || !out_len) {
        return false;
    }
    const size_t key_len = strlen(key);
    const char *p = line;
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        const char *token = p;
        while (*p && *p != ' ') {
            ++p;
        }
        const size_t token_len = (size_t)(p - token);
        if (token_len > key_len + 1U &&
            strncmp(token, key, key_len) == 0 &&
            token[key_len] == '=') {
            *out_value = token + key_len + 1U;
            *out_len = token_len - key_len - 1U;
            return true;
        }
    }
    return false;
}

static bool copy_token_value(const char *line, const char *key, char *dest, size_t dest_size)
{
    const char *value = NULL;
    size_t value_len = 0;
    if (!dest || dest_size == 0 ||
        !token_value_span(line, key, &value, &value_len) ||
        value_len + 1U > dest_size) {
        return false;
    }
    memcpy(dest, value, value_len);
    dest[value_len] = '\0';
    return true;
}

static bool parse_bool_token(const char *line, const char *key, bool *out_value)
{
    const char *value = NULL;
    size_t value_len = 0;
    if (!out_value || !token_value_span(line, key, &value, &value_len)) {
        return false;
    }
    if (value_len == 1U && value[0] == '1') {
        *out_value = true;
        return true;
    }
    if (value_len == 1U && value[0] == '0') {
        *out_value = false;
        return true;
    }
    return false;
}

static bool parse_u32_token(const char *line, const char *key, uint32_t *out_value)
{
    const char *value = NULL;
    size_t value_len = 0;
    if (!out_value || !token_value_span(line, key, &value, &value_len) || value_len == 0) {
        return false;
    }
    uint32_t parsed = 0;
    for (size_t i = 0; i < value_len; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
        const uint32_t digit = (uint32_t)(value[i] - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *out_value = parsed;
    return true;
}

static void parse_word_token(const char *line, const char *key, char *dest, size_t dest_size)
{
    if (!copy_token_value(line, key, dest, dest_size) && dest && dest_size > 0) {
        dest[0] = '\0';
    }
}

static void init_sd_status(d1l_rp2040_sd_status_t *status, esp_err_t err)
{
    memset(status, 0, sizeof(*status));
    status->bridge_ready = s_status.uart_ready;
    status->last_error = err;
    snprintf(status->state, sizeof(status->state), "%s",
             s_status.uart_ready ? "protocol_pending" : "bridge_unavailable");
    snprintf(status->filesystem, sizeof(status->filesystem), "unknown");
    snprintf(status->note, sizeof(status->note), "%s",
             s_status.uart_ready ?
             "RP2040 UART is ready but DeskOS SD status protocol has not answered" :
             "RP2040 UART bridge is unavailable");
    snprintf(status->probe_power, sizeof(status->probe_power), "unknown");
    snprintf(status->probe_mode, sizeof(status->probe_mode), "unknown");
}

static void init_ping(d1l_rp2040_ping_t *ping, esp_err_t err)
{
    memset(ping, 0, sizeof(*ping));
    ping->bridge_ready = s_status.uart_ready;
    ping->last_error = err;
    snprintf(ping->note, sizeof(ping->note), "%s",
             s_status.uart_ready ?
             "RP2040 UART is ready but DeskOS ping has not answered" :
             "RP2040 UART bridge is unavailable");
}

static void init_sd_diag(d1l_rp2040_sd_diag_t *diag, esp_err_t err)
{
    memset(diag, 0, sizeof(*diag));
    diag->bridge_ready = s_status.uart_ready;
    diag->last_error = err;
    snprintf(diag->pins, sizeof(diag->pins), "unknown");
    snprintf(diag->selected_power, sizeof(diag->selected_power), "unknown");
    snprintf(diag->selected_mode, sizeof(diag->selected_mode), "unknown");
    snprintf(diag->note, sizeof(diag->note), "%s",
             s_status.uart_ready ?
             "RP2040 UART is ready but SD diagnostics have not answered" :
             "RP2040 UART bridge is unavailable");
}

static void init_file_result(d1l_rp2040_file_result_t *result, esp_err_t err)
{
    memset(result, 0, sizeof(*result));
    result->bridge_ready = s_status.uart_ready;
    result->last_error = err;
    snprintf(result->op, sizeof(result->op), "unknown");
    if (err != ESP_OK) {
        snprintf(result->err, sizeof(result->err), "local_error");
        snprintf(result->note, sizeof(result->note), "local_error");
    }
}

static void init_stock_probe(d1l_rp2040_stock_probe_t *probe, esp_err_t err)
{
    memset(probe, 0, sizeof(*probe));
    probe->bridge_ready = s_status.uart_ready;
    probe->last_error = err;
    snprintf(probe->sent_cobs_hex, sizeof(probe->sent_cobs_hex), "02A500");
    snprintf(probe->note, sizeof(probe->note), "%s",
             s_status.uart_ready ?
             "Seeed stock RP2040 model-title probe has not answered" :
             "RP2040 UART bridge is unavailable");
}

static void append_stock_probe_byte(d1l_rp2040_stock_probe_t *probe, uint8_t byte)
{
    static const char hex[] = "0123456789ABCDEF";
    const uint32_t index = probe->bytes_read;
    probe->bytes_read++;
    if (index >= D1L_RP2040_STOCK_PROBE_MAX_BYTES) {
        probe->response_truncated = true;
        return;
    }
    probe->response_hex[index * 2U] = hex[(byte >> 4) & 0x0FU];
    probe->response_hex[(index * 2U) + 1U] = hex[byte & 0x0FU];
    probe->response_hex[(index * 2U) + 2U] = '\0';
    probe->response_ascii[index] = (byte >= 0x20U && byte <= 0x7EU) ? (char)byte : '.';
    probe->response_ascii[index + 1U] = '\0';
    if (byte == '{') {
        probe->json_seen = true;
    }
}

static bool supported_rp2040_baud(uint32_t baud)
{
    switch (baud) {
    case 9600U:
    case 38400U:
    case 57600U:
    case 115200U:
    case 230400U:
    case 460800U:
    case 921600U:
    case 1000000U:
        return true;
    default:
        return false;
    }
}

static void copy_limited_text(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }
    if (!src) {
        dest[0] = '\0';
        return;
    }
    size_t copy_len = strnlen(src, dest_size - 1U);
    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
}

static bool is_path_char(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-' || c == '/';
}

static bool validate_relative_path(const char *path)
{
    if (!path) {
        return false;
    }
    const size_t len = strlen(path);
    if (len == 0 || len > D1L_RP2040_FILE_PATH_MAX ||
        path[0] == '/' || path[len - 1U] == '/' ||
        strstr(path, "..") || strstr(path, "//")) {
        return false;
    }
    const char *segment = path;
    for (size_t i = 0; i <= len; ++i) {
        const char c = path[i];
        if (c == '/' || c == '\0') {
            const size_t segment_len = (size_t)(&path[i] - segment);
            if (segment_len == 0 ||
                (segment_len == 1U && segment[0] == '.') ||
                (segment_len == 2U && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            segment = &path[i + 1U];
            continue;
        }
        if (!is_path_char(c)) {
            return false;
        }
    }
    return true;
}

static uint32_t crc32_bytes(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (uint32_t)(-((int32_t)(crc & 1U)));
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return ~crc;
}

static void crc32_hex(uint32_t crc, char out[9])
{
    snprintf(out, 9, "%08lX", (unsigned long)crc);
}

static size_t base64url_encoded_len(size_t len)
{
    if (len == 0) {
        return 0;
    }
    const size_t padded = ((len + 2U) / 3U) * 4U;
    const size_t pad = (3U - (len % 3U)) % 3U;
    return padded - pad;
}

static bool base64url_encode(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const size_t encoded_len = base64url_encoded_len(len);
    if (!out || out_size <= encoded_len || (!data && len > 0)) {
        return false;
    }
    size_t used = 0;
    for (size_t i = 0; i < len; i += 3U) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1U < len) ? data[i + 1U] : 0U;
        const uint32_t b2 = (i + 2U < len) ? data[i + 2U] : 0U;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out[used++] = table[(triple >> 18) & 0x3FU];
        out[used++] = table[(triple >> 12) & 0x3FU];
        if (i + 1U < len) {
            out[used++] = table[(triple >> 6) & 0x3FU];
        }
        if (i + 2U < len) {
            out[used++] = table[triple & 0x3FU];
        }
    }
    out[used] = '\0';
    return used == encoded_len;
}

static bool encode_path(const char *path, char *encoded, size_t encoded_size)
{
    return validate_relative_path(path) &&
           base64url_encode((const uint8_t *)path, strlen(path), encoded, encoded_size);
}

static esp_err_t exchange_prefixed_line_internal(const char *command, size_t command_len,
                                                 const char *const *prefixes, size_t prefix_count,
                                                 char *line, size_t line_size,
                                                 uint32_t timeout_ms,
                                                 bool extend_timeout_on_rx,
                                                 bool caller_holds_lock,
                                                 bool *response_truncated,
                                                 const char *progress_prefix,
                                                 char *last_progress,
                                                 size_t last_progress_size)
{
    if (!s_status.uart_ready) {
        return s_status.init_result;
    }
    if (!command || command_len == 0 || !prefixes || prefix_count == 0 ||
        !line || line_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    if (response_truncated) {
        *response_truncated = false;
    }
    if (last_progress && last_progress_size > 0) {
        last_progress[0] = '\0';
    }

    const int64_t timeout_us = (int64_t)timeout_ms * 1000LL;
    int64_t deadline_us = 0;
    size_t used = 0;
    bool dropping = false;
    bool lock_acquired = false;
    esp_err_t ret = ESP_OK;
    if (!caller_holds_lock) {
        ret = take_bridge_lock(timeout_ms);
        if (ret != ESP_OK) {
            return ret;
        }
        lock_acquired = true;
    }
    uart_flush_input((uart_port_t)s_status.uart_port);
    const int written = uart_write_bytes((uart_port_t)s_status.uart_port, command, command_len);
    if (written != (int)command_len) {
        ret = ESP_FAIL;
        goto done;
    }

    deadline_us = esp_timer_get_time() + timeout_us;
    while (esp_timer_get_time() < deadline_us) {
        uint8_t ch = 0;
        int len = uart_read_bytes((uart_port_t)s_status.uart_port, &ch, 1, pdMS_TO_TICKS(10));
        if (len <= 0) {
            continue;
        }
        if (extend_timeout_on_rx) {
            deadline_us = esp_timer_get_time() + timeout_us;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (!dropping) {
                line[used] = '\0';
                for (size_t i = 0; i < prefix_count; ++i) {
                    if (line_has_prefix(line, prefixes[i])) {
                        ret = ESP_OK;
                        goto done;
                    }
                }
                if (progress_prefix && line_has_prefix(line, progress_prefix) &&
                    last_progress && last_progress_size > 0) {
                    const char *step = NULL;
                    size_t step_len = 0;
                    if (token_value_span(line, "step", &step, &step_len) &&
                        step_len + 1U <= last_progress_size) {
                        memcpy(last_progress, step, step_len);
                        last_progress[step_len] = '\0';
                    } else {
                        snprintf(last_progress, last_progress_size, "progress");
                    }
                }
            }
            used = 0;
            dropping = false;
            continue;
        }
        if (dropping) {
            continue;
        }
        if (used + 1U < line_size) {
            line[used++] = (char)ch;
        } else {
            used = 0;
            dropping = true;
            if (response_truncated) {
                *response_truncated = true;
            }
        }
    }
    ret = ESP_ERR_TIMEOUT;

done:
    if (lock_acquired) {
        give_bridge_lock();
    }
    return ret;
}

static esp_err_t exchange_prefixed_line(const char *command, size_t command_len,
                                        const char *const *prefixes, size_t prefix_count,
                                        char *line, size_t line_size, uint32_t timeout_ms,
                                        bool *response_truncated)
{
    return exchange_prefixed_line_internal(command, command_len, prefixes, prefix_count,
                                           line, line_size, timeout_ms, false, false,
                                           response_truncated, NULL, NULL, 0);
}

static esp_err_t exchange_prefixed_line_while_locked(
    const char *command, size_t command_len,
    const char *const *prefixes, size_t prefix_count,
    char *line, size_t line_size, uint32_t timeout_ms,
    bool *response_truncated)
{
    return exchange_prefixed_line_internal(
        command, command_len, prefixes, prefix_count,
        line, line_size, timeout_ms, false, true,
        response_truncated, NULL, NULL, 0);
}

static esp_err_t parse_sd_line_with_prefix(const char *line,
                                           const char *prefix,
                                           d1l_rp2040_sd_status_t *status)
{
    d1l_rp2040_sd_reply_t reply = {0};
    if (!status || !d1l_rp2040_sd_reply_parse(line, prefix, &reply)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    init_sd_status(status, ESP_OK);
    status->protocol_supported = true;
    memcpy(status->state, reply.state, sizeof(status->state));
    memcpy(status->filesystem, reply.filesystem, sizeof(status->filesystem));
    memcpy(status->note, reply.note, sizeof(status->note));
    memcpy(status->probe_power, reply.probe_power, sizeof(status->probe_power));
    memcpy(status->probe_mode, reply.probe_mode, sizeof(status->probe_mode));
    status->card_present = reply.card_present;
    status->filesystem_mounted = reply.filesystem_mounted;
    status->deskos_root_ready = reply.deskos_root_ready;
    status->needs_fat32 = reply.needs_fat32;
    status->file_ops_supported = reply.file_ops_supported;
    status->atomic_rename_supported = reply.atomic_rename_supported;
    status->capacity_kb = reply.capacity_kb;
    status->free_kb = reply.free_kb;
    status->file_line_max = reply.file_line_max;
    status->file_chunk_max = reply.file_chunk_max;
    status->path_max = reply.path_max;
    status->probe_error = reply.probe_error;
    status->probe_data = reply.probe_data;
    status->mount_error = reply.mount_error;
    status->mount_data = reply.mount_data;

    bool legacy_format_required = false;
    if (parse_bool_token(line, "format_required", &legacy_format_required) &&
        legacy_format_required) {
        status->needs_fat32 = true;
    }

    const bool probe_rejected_card =
        strcmp(status->note, "sd_probe_rejected_card") == 0 ||
        (strcmp(status->state, "error") == 0 &&
         (status->probe_error == 3U || status->probe_error == 4U));
    if (probe_rejected_card) {
        status->needs_fat32 = false;
    } else if (status->card_present && !status->filesystem_mounted) {
        status->needs_fat32 = true;
    }
    status->data_ready = status->card_present &&
                         status->filesystem_mounted &&
                         status->deskos_root_ready &&
                         !status->needs_fat32;
    if (status->state[0] == '\0') {
        snprintf(status->state, sizeof(status->state), "%s",
                 status->data_ready ? "ready" :
                 status->needs_fat32 ? "not_fat32_or_unmountable" :
                 status->card_present ? "deskos_manifest_invalid" : "no_card");
    }
    if (status->filesystem[0] == '\0') {
        snprintf(status->filesystem, sizeof(status->filesystem), "unknown");
    }
    if (status->note[0] == '\0') {
        const bool mount_failed_with_diag = status->card_present &&
                                           !status->filesystem_mounted &&
                                           (status->mount_error != 0U ||
                                            status->mount_data != 0U);
        snprintf(status->note, sizeof(status->note), "%s",
                 status->data_ready ? "SD card is ready for DeskOS data" :
                 mount_failed_with_diag ? "RP2040 SD filesystem mount failed; inspect firmware mount diagnostics" :
                 probe_rejected_card ? "RP2040 SD probe rejected the card init response; inspect CMD0/CMD8 diagnostics" :
                 status->needs_fat32 ? "Prepare a FAT32 card on a computer before using SD storage" :
                 "SD card is not ready for DeskOS data");
    }
    return ESP_OK;
}

static esp_err_t parse_sd_status_line(const char *line, d1l_rp2040_sd_status_t *status)
{
    return parse_sd_line_with_prefix(line, D1L_RP2040_SD_REPLY_PREFIX, status);
}

static esp_err_t parse_sd_mount_line(const char *line, d1l_rp2040_sd_status_t *status)
{
    return parse_sd_line_with_prefix(line, D1L_RP2040_SD_MOUNT_REPLY_PREFIX, status);
}

static esp_err_t parse_ping_line(const char *line, d1l_rp2040_ping_t *ping)
{
    if (!line || !ping || !line_has_prefix(line, D1L_RP2040_PING_REPLY_PREFIX)) {
        return ESP_FAIL;
    }

    init_ping(ping, ESP_OK);
    ping->protocol_supported = true;
    (void)parse_u32_token(line, "v", &ping->protocol_version);
    (void)parse_u32_token(line, "file_line_max", &ping->file_line_max);
    (void)parse_u32_token(line, "file_chunk_max", &ping->file_chunk_max);
    (void)parse_u32_token(line, "path_max", &ping->path_max);
    (void)parse_bool_token(line, "atomic_rename", &ping->atomic_rename_supported);
    (void)parse_bool_token(
        line, "stream_write", &ping->stream_write_supported);
    (void)parse_bool_token(line, "sd_touch", &ping->sd_touched);
    snprintf(ping->note, sizeof(ping->note), "%s",
             ping->sd_touched ? "ping_touched_sd" : "ok_no_sd_touch");
    return ESP_OK;
}

static esp_err_t parse_sd_diag_line(const char *line, d1l_rp2040_sd_diag_t *diag)
{
    if (!line || !diag || !line_has_prefix(line, D1L_RP2040_SD_DIAG_REPLY_PREFIX)) {
        return ESP_FAIL;
    }

    diag->protocol_supported = true;
    if (line != diag->raw_line) {
        snprintf(diag->raw_line, sizeof(diag->raw_line), "%s", line);
    }
    parse_word_token(line, "pins", diag->pins, sizeof(diag->pins));
    parse_word_token(line, "selected_power", diag->selected_power, sizeof(diag->selected_power));
    parse_word_token(line, "selected_mode", diag->selected_mode, sizeof(diag->selected_mode));
    (void)parse_bool_token(line, "mount_selected", &diag->mount_selected);
    (void)parse_u32_token(line, "hz", &diag->spi_hz);
    (void)parse_bool_token(line, "hd_p", &diag->high_dedicated_present);
    (void)parse_u32_token(line, "hd_e", &diag->high_dedicated_error);
    (void)parse_u32_token(line, "hd_d", &diag->high_dedicated_data);
    (void)parse_u32_token(line, "hd_kb", &diag->high_dedicated_capacity_kb);
    (void)parse_bool_token(line, "hs_p", &diag->high_shared_present);
    (void)parse_u32_token(line, "hs_e", &diag->high_shared_error);
    (void)parse_u32_token(line, "hs_d", &diag->high_shared_data);
    (void)parse_u32_token(line, "hs_kb", &diag->high_shared_capacity_kb);
    (void)parse_bool_token(line, "ld_p", &diag->low_dedicated_present);
    (void)parse_u32_token(line, "ld_e", &diag->low_dedicated_error);
    (void)parse_u32_token(line, "ld_d", &diag->low_dedicated_data);
    (void)parse_u32_token(line, "ld_kb", &diag->low_dedicated_capacity_kb);
    (void)parse_bool_token(line, "ls_p", &diag->low_shared_present);
    (void)parse_u32_token(line, "ls_e", &diag->low_shared_error);
    (void)parse_u32_token(line, "ls_d", &diag->low_shared_data);
    (void)parse_u32_token(line, "ls_kb", &diag->low_shared_capacity_kb);
    snprintf(diag->note, sizeof(diag->note), "%s",
             diag->mount_selected ? "selected_probe_mounted" :
             (diag->high_dedicated_present || diag->high_shared_present ||
              diag->low_dedicated_present || diag->low_shared_present) ?
             "raw_card_present_mount_failed" : "raw_card_not_detected");
    return ESP_OK;
}

static uint16_t next_file_request_id(void)
{
    const uint16_t id = s_file_request_id;
    ++s_file_request_id;
    if (s_file_request_id == 0) {
        s_file_request_id = 1;
    }
    return id;
}

static esp_err_t send_file_command_internal(
    const char *command, size_t command_len,
    uint16_t request_id, const char *expected_op,
    uint8_t *out_data, size_t out_data_size,
    d1l_rp2040_file_result_t *out_result,
    uint32_t timeout_ms, bool caller_holds_lock)
{
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_status.uart_ready) {
        init_file_result(out_result, s_status.init_result);
        return s_status.init_result;
    }

    const char *prefixes[] = {D1L_RP2040_FILE_PREFIX};
    char line[D1L_RP2040_LINE_BUFFER_SIZE];
    bool truncated = false;
    init_file_result(out_result, ESP_ERR_TIMEOUT);
    esp_err_t ret = caller_holds_lock ?
        exchange_prefixed_line_while_locked(
            command, command_len, prefixes, 1, line,
            sizeof(line), timeout_ms, &truncated) :
        exchange_prefixed_line(command, command_len, prefixes, 1,
                               line, sizeof(line), timeout_ms,
                               &truncated);
    if (ret != ESP_OK) {
        out_result->last_error = ret;
        out_result->response_truncated = truncated;
        snprintf(out_result->note, sizeof(out_result->note),
                 ret == ESP_ERR_TIMEOUT ? "timeout" : "query_failed");
        return ret;
    }
    ret = d1l_rp2040_file_reply_parse(
        line, request_id, expected_op, out_data, out_data_size, out_result);
    out_result->response_truncated = truncated;
    return ret;
}

static esp_err_t send_file_command(const char *command, size_t command_len,
                                   uint16_t request_id, const char *expected_op,
                                   uint8_t *out_data, size_t out_data_size,
                                   d1l_rp2040_file_result_t *out_result,
                                   uint32_t timeout_ms)
{
    return send_file_command_internal(
        command, command_len, request_id, expected_op,
        out_data, out_data_size, out_result, timeout_ms, false);
}

static esp_err_t send_file_command_while_locked(
    const char *command, size_t command_len,
    uint16_t request_id, const char *expected_op,
    d1l_rp2040_file_result_t *out_result,
    uint32_t timeout_ms)
{
    return send_file_command_internal(
        command, command_len, request_id, expected_op,
        NULL, 0U, out_result, timeout_ms, true);
}

esp_err_t d1l_rp2040_bridge_init(void)
{
    ensure_bridge_mutex();

    const d1l_rp2040_pins_t *pins = d1l_rp2040_pins();
    s_status.uart_port = pins->uart_port;
    s_status.tx_gpio = pins->esp_tx_gpio;
    s_status.rx_gpio = pins->esp_rx_gpio;
    s_status.baud_rate = pins->baud_rate;
    s_status.reset_expander_pin = pins->expander_reset;

    uart_config_t uart_config = {
        .baud_rate = pins->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install((uart_port_t)pins->uart_port, D1L_RP2040_UART_BUF_SIZE, 0, 0, NULL, 0);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        ret = uart_param_config((uart_port_t)pins->uart_port, &uart_config);
    }
    if (ret == ESP_OK) {
        ret = uart_set_pin((uart_port_t)pins->uart_port, pins->esp_tx_gpio, pins->esp_rx_gpio,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

    s_status.init_result = ret;
    s_status.uart_ready = (ret == ESP_OK);
    return ret;
}

esp_err_t d1l_rp2040_bridge_status(d1l_rp2040_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_status.uart_ready) {
        size_t buffered = 0;
        esp_err_t ret = uart_get_buffered_data_len((uart_port_t)s_status.uart_port, &buffered);
        if (ret == ESP_OK) {
            s_status.buffered_bytes = (uint32_t)buffered;
        } else {
            s_status.init_result = ret;
        }
    }
    *out_status = s_status;
    return s_status.init_result;
}

esp_err_t d1l_rp2040_bridge_set_baud(uint32_t baud)
{
    if (!supported_rp2040_baud(baud)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_status.uart_ready) {
        return s_status.init_result;
    }
    esp_err_t ret = take_bridge_lock(1000U);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = uart_set_baudrate((uart_port_t)s_status.uart_port, baud);
    if (ret != ESP_OK) {
        s_status.init_result = ret;
        s_status.uart_ready = false;
        goto done;
    }
    s_status.baud_rate = (int)baud;
    uart_flush_input((uart_port_t)s_status.uart_port);
    ret = ESP_OK;

done:
    give_bridge_lock();
    return ret;
}

esp_err_t d1l_rp2040_bridge_reset(uint32_t hold_ms, uint32_t settle_ms)
{
    const d1l_rp2040_pins_t *pins = d1l_rp2040_pins();
    const uint32_t hold = hold_ms > 0 ? hold_ms : 100U;
    const uint32_t settle = settle_ms > 0 ? settle_ms : 8000U;

    esp_err_t ret = take_bridge_lock(hold + settle + 1000U);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = tca9535_set_direction(pins->expander_reset, true);
    if (ret != ESP_OK) {
        goto done;
    }
    ret = pulse_rp2040_reset(pins, hold);
    if (ret != ESP_OK) {
        goto done;
    }

    if (s_status.uart_ready) {
        uart_flush_input((uart_port_t)s_status.uart_port);
    }
    vTaskDelay(pdMS_TO_TICKS(settle));
    ret = ESP_OK;

done:
    give_bridge_lock();
    return ret;
}

esp_err_t d1l_rp2040_bridge_double_reset(uint32_t hold_ms, uint32_t gap_ms, uint32_t settle_ms)
{
    const d1l_rp2040_pins_t *pins = d1l_rp2040_pins();
    const uint32_t hold = hold_ms > 0 ? hold_ms : 50U;
    const uint32_t gap = gap_ms > 0 ? gap_ms : 150U;
    const uint32_t settle = settle_ms > 0 ? settle_ms : 1500U;

    esp_err_t ret = take_bridge_lock((hold * 2U) + gap + settle + 1000U);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = tca9535_set_direction(pins->expander_reset, true);
    if (ret != ESP_OK) {
        goto done;
    }
    ret = pulse_rp2040_reset(pins, hold);
    if (ret != ESP_OK) {
        goto done;
    }
    vTaskDelay(pdMS_TO_TICKS(gap));
    ret = pulse_rp2040_reset(pins, hold);
    if (ret != ESP_OK) {
        goto done;
    }

    if (s_status.uart_ready) {
        uart_flush_input((uart_port_t)s_status.uart_port);
    }
    vTaskDelay(pdMS_TO_TICKS(settle));
    ret = ESP_OK;

done:
    give_bridge_lock();
    return ret;
}

esp_err_t d1l_rp2040_bridge_ping(d1l_rp2040_ping_t *out_ping, uint32_t timeout_ms)
{
    if (out_ping == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_status.uart_ready) {
        init_ping(out_ping, s_status.init_result);
        return s_status.init_result;
    }

    const char *prefixes[] = {D1L_RP2040_PING_REPLY_PREFIX};
    char line[D1L_RP2040_LINE_BUFFER_SIZE];
    bool truncated = false;
    init_ping(out_ping, ESP_ERR_TIMEOUT);
    esp_err_t ret = exchange_prefixed_line(D1L_RP2040_PING_QUERY,
                                           strlen(D1L_RP2040_PING_QUERY),
                                           prefixes, 1, line, sizeof(line),
                                           timeout_ms, &truncated);
    out_ping->response_truncated = truncated;
    if (ret != ESP_OK) {
        out_ping->last_error = ret;
        snprintf(out_ping->note, sizeof(out_ping->note),
                 ret == ESP_ERR_TIMEOUT ? "timeout" : "query_failed");
        return ret;
    }
    ret = parse_ping_line(line, out_ping);
    out_ping->response_truncated = truncated;
    out_ping->last_error = ret;
    return ret;
}

esp_err_t d1l_rp2040_bridge_enter_bootloader(uint32_t timeout_ms)
{
    if (!s_status.uart_ready) {
        return s_status.init_result;
    }

    const char *prefixes[] = {D1L_RP2040_BOOTLOADER_REPLY_PREFIX};
    char line[D1L_RP2040_LINE_BUFFER_SIZE];
    bool truncated = false;
    esp_err_t ret = exchange_prefixed_line(D1L_RP2040_BOOTLOADER_QUERY,
                                           strlen(D1L_RP2040_BOOTLOADER_QUERY),
                                           prefixes, 1, line, sizeof(line),
                                           timeout_ms, &truncated);
    if (ret != ESP_OK) {
        return ret;
    }
    if (truncated) {
        return ESP_ERR_INVALID_SIZE;
    }
    bool ok = false;
    if (!parse_bool_token(line, "ok", &ok) || !ok) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t d1l_rp2040_bridge_stock_probe(d1l_rp2040_stock_probe_t *out_probe, uint32_t timeout_ms)
{
    if (out_probe == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_status.uart_ready) {
        init_stock_probe(out_probe, s_status.init_result);
        return s_status.init_result;
    }

    static const uint8_t seeed_model_title_query[] = {0x02U, 0xA5U, 0x00U};
    init_stock_probe(out_probe, ESP_ERR_TIMEOUT);
    int64_t deadline_us = 0;
    bool saw_close_brace = false;
    esp_err_t ret = take_bridge_lock(timeout_ms);
    if (ret != ESP_OK) {
        out_probe->last_error = ret;
        snprintf(out_probe->note, sizeof(out_probe->note),
                 "could not lock RP2040 UART for Seeed stock model-title probe");
        return ret;
    }
    uart_flush_input((uart_port_t)s_status.uart_port);
    const int written = uart_write_bytes((uart_port_t)s_status.uart_port,
                                         (const char *)seeed_model_title_query,
                                         sizeof(seeed_model_title_query));
    if (written != (int)sizeof(seeed_model_title_query)) {
        out_probe->last_error = ESP_FAIL;
        snprintf(out_probe->note, sizeof(out_probe->note),
                 "could not write Seeed stock model-title COBS probe");
        ret = ESP_FAIL;
        goto done;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while (esp_timer_get_time() < deadline_us) {
        uint8_t ch = 0;
        int len = uart_read_bytes((uart_port_t)s_status.uart_port, &ch, 1, pdMS_TO_TICKS(10));
        if (len <= 0) {
            continue;
        }
        out_probe->response_seen = true;
        append_stock_probe_byte(out_probe, ch);
        if (ch == '}') {
            saw_close_brace = true;
            break;
        }
    }

    if (!out_probe->response_seen) {
        out_probe->last_error = ESP_ERR_TIMEOUT;
        snprintf(out_probe->note, sizeof(out_probe->note),
                 "Seeed stock model-title COBS probe timed out");
        ret = ESP_ERR_TIMEOUT;
        goto done;
    }

    out_probe->last_error = ESP_OK;
    if (saw_close_brace && out_probe->json_seen) {
        snprintf(out_probe->note, sizeof(out_probe->note),
                 "Seeed stock RP2040 JSON response captured");
    } else {
        snprintf(out_probe->note, sizeof(out_probe->note),
                 "Non-empty RP2040 response captured");
    }
    ret = ESP_OK;

done:
    give_bridge_lock();
    return ret;
}

esp_err_t d1l_rp2040_bridge_baud_probe(d1l_rp2040_baud_probe_t *out_probe, uint32_t timeout_ms)
{
    if (out_probe == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->bridge_ready = s_status.uart_ready;
    out_probe->original_baud = (uint32_t)s_status.baud_rate;
    out_probe->last_error = ESP_ERR_NOT_FOUND;
    if (!s_status.uart_ready) {
        out_probe->last_error = s_status.init_result;
        return s_status.init_result;
    }

    static const uint32_t bauds[] = {
        921600U,
        460800U,
        230400U,
        115200U,
        57600U,
        38400U,
        9600U,
        1000000U,
    };
    const uint32_t per_probe_timeout = timeout_ms > 0 ? timeout_ms : 700U;
    const uint32_t original_baud = (uint32_t)s_status.baud_rate;

    for (size_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]) &&
                       i < D1L_RP2040_BAUD_PROBE_MAX_RESULTS; ++i) {
        d1l_rp2040_baud_probe_result_t *result = &out_probe->results[i];
        result->baud = bauds[i];
        out_probe->tested_count++;

        esp_err_t ret = d1l_rp2040_bridge_set_baud(bauds[i]);
        if (ret != ESP_OK) {
            result->ping_error = ret;
            result->stock_error = ret;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(25));

        d1l_rp2040_ping_t ping = {0};
        ret = d1l_rp2040_bridge_ping(&ping, per_probe_timeout);
        result->ping_error = ret;
        result->deskos_ping_ok = (ret == ESP_OK && ping.protocol_supported);
        if (result->deskos_ping_ok && !out_probe->found_deskos) {
            out_probe->found_deskos = true;
            out_probe->selected_baud = bauds[i];
            out_probe->last_error = ESP_OK;
        }

        d1l_rp2040_stock_probe_t stock = {0};
        ret = d1l_rp2040_bridge_stock_probe(&stock, per_probe_timeout);
        result->stock_error = ret;
        result->stock_response_seen = stock.response_seen;
        result->stock_json_seen = stock.json_seen;
        result->stock_response_truncated = stock.response_truncated;
        result->stock_bytes_read = stock.bytes_read;
        copy_limited_text(result->stock_response_ascii,
                          sizeof(result->stock_response_ascii),
                          stock.response_ascii);
        copy_limited_text(result->stock_response_hex,
                          sizeof(result->stock_response_hex),
                          stock.response_hex);
        if (stock.response_seen && !out_probe->found_stock) {
            out_probe->found_stock = true;
            if (!out_probe->found_deskos) {
                out_probe->selected_baud = bauds[i];
                out_probe->last_error = ESP_OK;
            }
        }
    }

    const esp_err_t restore_ret = d1l_rp2040_bridge_set_baud(original_baud);
    if (restore_ret != ESP_OK) {
        out_probe->last_error = restore_ret;
        return restore_ret;
    }
    return ESP_OK;
}

esp_err_t d1l_rp2040_bridge_probe_sd(d1l_rp2040_sd_status_t *out_status, uint32_t timeout_ms)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_status.uart_ready) {
        init_sd_status(out_status, s_status.init_result);
        return s_status.init_result;
    }

    const char *prefixes[] = {D1L_RP2040_SD_REPLY_PREFIX};
    char line[D1L_RP2040_LINE_BUFFER_SIZE];
    bool truncated = false;
    init_sd_status(out_status, ESP_ERR_TIMEOUT);
    esp_err_t ret = exchange_prefixed_line(D1L_RP2040_SD_QUERY,
                                           strlen(D1L_RP2040_SD_QUERY),
                                           prefixes, 1, line, sizeof(line),
                                           timeout_ms, &truncated);
    out_status->response_truncated = truncated;
    if (ret != ESP_OK) {
        out_status->last_error = ret;
        if (ret != ESP_ERR_TIMEOUT) {
            snprintf(out_status->state, sizeof(out_status->state), "query_failed");
            snprintf(out_status->note, sizeof(out_status->note),
                     "Could not write SD status query to RP2040 UART");
        }
        return ret;
    }
    ret = parse_sd_status_line(line, out_status);
    out_status->response_truncated = truncated;
    out_status->last_error = ret;
    return ret;
}

esp_err_t d1l_rp2040_bridge_mount_sd(d1l_rp2040_sd_status_t *out_status, uint32_t timeout_ms)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_status.uart_ready) {
        init_sd_status(out_status, s_status.init_result);
        return s_status.init_result;
    }

    const char *prefixes[] = {D1L_RP2040_SD_MOUNT_REPLY_PREFIX};
    char line[D1L_RP2040_LINE_BUFFER_SIZE];
    bool truncated = false;
    init_sd_status(out_status, ESP_ERR_TIMEOUT);
    esp_err_t ret = exchange_prefixed_line(D1L_RP2040_SD_MOUNT_QUERY,
                                           strlen(D1L_RP2040_SD_MOUNT_QUERY),
                                           prefixes, 1, line, sizeof(line),
                                           timeout_ms, &truncated);
    out_status->response_truncated = truncated;
    if (ret != ESP_OK) {
        out_status->last_error = ret;
        if (ret != ESP_ERR_TIMEOUT) {
            snprintf(out_status->state, sizeof(out_status->state), "query_failed");
            snprintf(out_status->note, sizeof(out_status->note),
                     "Could not write SD mount query to RP2040 UART");
        }
        return ret;
    }
    ret = parse_sd_mount_line(line, out_status);
    out_status->response_truncated = truncated;
    out_status->last_error = ret;
    return ret;
}

esp_err_t d1l_rp2040_bridge_sd_diag(d1l_rp2040_sd_diag_t *out_diag, uint32_t timeout_ms)
{
    if (out_diag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_status.uart_ready) {
        init_sd_diag(out_diag, s_status.init_result);
        return s_status.init_result;
    }

    const char *prefixes[] = {D1L_RP2040_SD_DIAG_REPLY_PREFIX};
    bool truncated = false;
    init_sd_diag(out_diag, ESP_ERR_TIMEOUT);
    esp_err_t ret = exchange_prefixed_line(D1L_RP2040_SD_DIAG_QUERY,
                                            strlen(D1L_RP2040_SD_DIAG_QUERY),
                                            prefixes, 1, out_diag->raw_line, sizeof(out_diag->raw_line),
                                            timeout_ms, &truncated);
    out_diag->response_truncated = truncated;
    if (ret != ESP_OK) {
        out_diag->last_error = ret;
        snprintf(out_diag->note, sizeof(out_diag->note),
                 ret == ESP_ERR_TIMEOUT ? "timeout" : "query_failed");
        return ret;
    }
    ret = parse_sd_diag_line(out_diag->raw_line, out_diag);
    out_diag->response_truncated = truncated;
    out_diag->last_error = ret;
    return ret;
}

esp_err_t d1l_rp2040_bridge_file_stat(const char *path,
                                      d1l_rp2040_file_result_t *out_result,
                                      uint32_t timeout_ms)
{
    if (!path || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    char path64[D1L_RP2040_PATH64_MAX + 1U];
    if (!encode_path(path, path64, sizeof(path64))) {
        init_file_result(out_result, ESP_ERR_INVALID_ARG);
        snprintf(out_result->err, sizeof(out_result->err), "bad_path");
        snprintf(out_result->note, sizeof(out_result->note), "bad_path");
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t request_id = next_file_request_id();
    char command[D1L_RP2040_FILE_LINE_MAX + 2U];
    const int command_len = snprintf(command, sizeof(command),
                                     "%s v=1 id=%u op=stat path=%s\n",
                                     D1L_RP2040_FILE_PREFIX, (unsigned)request_id, path64);
    if (command_len <= 0 || (size_t)command_len >= sizeof(command)) {
        init_file_result(out_result, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    return send_file_command(command, (size_t)command_len, request_id, "stat",
                             NULL, 0, out_result, timeout_ms);
}

esp_err_t d1l_rp2040_bridge_file_read(const char *path,
                                      uint32_t offset,
                                      uint8_t *out_data,
                                      size_t max_len,
                                      d1l_rp2040_file_result_t *out_result,
                                      uint32_t timeout_ms)
{
    if (!path || !out_data || !out_result || max_len == 0 ||
        max_len > D1L_RP2040_FILE_CHUNK_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    char path64[D1L_RP2040_PATH64_MAX + 1U];
    if (!encode_path(path, path64, sizeof(path64))) {
        init_file_result(out_result, ESP_ERR_INVALID_ARG);
        snprintf(out_result->err, sizeof(out_result->err), "bad_path");
        snprintf(out_result->note, sizeof(out_result->note), "bad_path");
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t request_id = next_file_request_id();
    char command[D1L_RP2040_FILE_LINE_MAX + 2U];
    const int command_len = snprintf(command, sizeof(command),
                                     "%s v=1 id=%u op=read path=%s off=%lu len=%lu\n",
                                     D1L_RP2040_FILE_PREFIX, (unsigned)request_id, path64,
                                     (unsigned long)offset, (unsigned long)max_len);
    if (command_len <= 0 || (size_t)command_len >= sizeof(command)) {
        init_file_result(out_result, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t ret = send_file_command(
        command, (size_t)command_len, request_id, "read",
        out_data, max_len, out_result, timeout_ms);
    return ret == ESP_OK ? d1l_rp2040_file_reply_bind_read(
                              out_result, offset, max_len) : ret;
}

esp_err_t d1l_rp2040_bridge_file_write(const char *path,
                                       uint32_t offset,
                                       const uint8_t *data,
                                       size_t len,
                                       bool truncate,
                                       d1l_rp2040_file_result_t *out_result,
                                       uint32_t timeout_ms)
{
    if (!path || !out_result || (len > 0 && !data) || len > D1L_RP2040_FILE_CHUNK_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    char path64[D1L_RP2040_PATH64_MAX + 1U];
    char data64[D1L_RP2040_DATA64_MAX + 1U];
    char crc[9];
    if (!encode_path(path, path64, sizeof(path64)) ||
        !base64url_encode(data, len, data64, sizeof(data64))) {
        init_file_result(out_result, ESP_ERR_INVALID_ARG);
        snprintf(out_result->err, sizeof(out_result->err), "bad_path");
        snprintf(out_result->note, sizeof(out_result->note), "bad_path");
        return ESP_ERR_INVALID_ARG;
    }
    crc32_hex(crc32_bytes(data, len), crc);
    const uint16_t request_id = next_file_request_id();
    char command[D1L_RP2040_FILE_LINE_MAX + 2U];
    const int command_len = snprintf(command, sizeof(command),
                                     "%s v=1 id=%u op=write path=%s off=%lu len=%lu trunc=%u data=%s crc=%s\n",
                                     D1L_RP2040_FILE_PREFIX, (unsigned)request_id, path64,
                                     (unsigned long)offset, (unsigned long)len,
                                     truncate ? 1U : 0U, data64, crc);
    if (command_len <= 0 || (size_t)command_len >= sizeof(command)) {
        init_file_result(out_result, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t ret = send_file_command(
        command, (size_t)command_len, request_id, "write",
        NULL, 0, out_result, timeout_ms);
    return ret == ESP_OK ? d1l_rp2040_file_reply_bind_write(
                              out_result, offset, len) : ret;
}

static bool verified_put_continue(
    d1l_rp2040_file_continue_cb_t should_continue,
    void *continue_context)
{
    return !should_continue || should_continue(continue_context);
}

static bool verified_put_cleanup_confirmed(
    const d1l_rp2040_file_result_t *result)
{
    return result && result->removed_known && result->removed;
}

static void note_verified_put_cancelled(
    d1l_rp2040_file_result_t *result)
{
    if (!result) {
        return;
    }
    result->ok = false;
    result->cancelled = true;
    result->last_error = ESP_ERR_INVALID_STATE;
    snprintf(result->err, sizeof(result->err), "cancelled");
    snprintf(result->note, sizeof(result->note), "cancelled");
}

static esp_err_t abort_verified_put_while_locked(
    d1l_rp2040_file_result_t *out_result,
    uint32_t timeout_ms)
{
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t request_id = next_file_request_id();
    char command[D1L_RP2040_FILE_LINE_MAX + 2U];
    const int command_len = snprintf(
        command, sizeof(command),
        "%s v=1 id=%u op=put_abort\n",
        D1L_RP2040_FILE_PREFIX, (unsigned)request_id);
    if (command_len <= 0 ||
        (size_t)command_len >= sizeof(command)) {
        init_file_result(out_result, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t ret = send_file_command_while_locked(
        command, (size_t)command_len, request_id,
        "put_abort", out_result, timeout_ms);
    return ret == ESP_OK ?
        d1l_rp2040_file_reply_bind_put_abort(out_result) : ret;
}

esp_err_t d1l_rp2040_bridge_file_write_verified(
    const char *path,
    const uint8_t *data,
    size_t len,
    uint32_t expected_crc32,
    d1l_rp2040_file_continue_cb_t should_continue,
    void *continue_context,
    d1l_rp2040_file_result_t *out_result,
    uint32_t timeout_ms)
{
    if (!path || !out_result || (!data && len > 0U) ||
        len > UINT32_MAX || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    char path64[D1L_RP2040_PATH64_MAX + 1U];
    if (!encode_path(path, path64, sizeof(path64))) {
        init_file_result(out_result, ESP_ERR_INVALID_ARG);
        snprintf(out_result->err, sizeof(out_result->err), "bad_path");
        snprintf(out_result->note, sizeof(out_result->note), "bad_path");
        return ESP_ERR_INVALID_ARG;
    }
    if (crc32_bytes(data, len) != expected_crc32) {
        init_file_result(out_result, ESP_ERR_INVALID_CRC);
        snprintf(out_result->err, sizeof(out_result->err),
                 "crc_mismatch");
        snprintf(out_result->note, sizeof(out_result->note),
                 "crc_mismatch");
        return ESP_ERR_INVALID_CRC;
    }
    if (!verified_put_continue(
            should_continue, continue_context)) {
        init_file_result(out_result, ESP_ERR_INVALID_STATE);
        note_verified_put_cancelled(out_result);
        return out_result->last_error;
    }
    if (!s_status.uart_ready) {
        init_file_result(out_result, s_status.init_result);
        return s_status.init_result;
    }

    esp_err_t ret = take_bridge_lock(timeout_ms);
    if (ret != ESP_OK) {
        init_file_result(out_result, ret);
        return ret;
    }

    const uint32_t expected_size = (uint32_t)len;
    char expected_crc[9];
    crc32_hex(expected_crc32, expected_crc);
    bool session_maybe_active = false;
    d1l_rp2040_file_result_t step_result = {0};

    const uint16_t begin_id = next_file_request_id();
    char command[D1L_RP2040_FILE_LINE_MAX + 2U];
    int command_len = snprintf(
        command, sizeof(command),
        "%s v=1 id=%u op=put_begin path=%s size=%lu crc=%s\n",
        D1L_RP2040_FILE_PREFIX, (unsigned)begin_id, path64,
        (unsigned long)expected_size, expected_crc);
    if (command_len <= 0 ||
        (size_t)command_len >= sizeof(command)) {
        init_file_result(&step_result, ESP_ERR_INVALID_SIZE);
        ret = ESP_ERR_INVALID_SIZE;
        goto verified_put_done;
    }
    ret = send_file_command_while_locked(
        command, (size_t)command_len, begin_id,
        "put_begin", &step_result, timeout_ms);
    session_maybe_active = ret != ESP_ERR_NOT_SUPPORTED;
    if (verified_put_cleanup_confirmed(&step_result)) {
        session_maybe_active = false;
    }
    if (ret == ESP_OK) {
        ret = d1l_rp2040_file_reply_bind_put_begin(
            &step_result, expected_size, expected_crc32);
    }
    if (ret != ESP_OK) {
        goto verified_put_done;
    }

    for (size_t offset = 0U; offset < len;) {
        if (!verified_put_continue(
                should_continue, continue_context)) {
            note_verified_put_cancelled(&step_result);
            ret = step_result.last_error;
            goto verified_put_done;
        }
        const size_t remaining = len - offset;
        const size_t chunk_len =
            remaining < D1L_RP2040_FILE_CHUNK_MAX ?
                remaining : D1L_RP2040_FILE_CHUNK_MAX;
        char data64[D1L_RP2040_DATA64_MAX + 1U];
        char chunk_crc[9];
        if (!base64url_encode(
                &data[offset], chunk_len,
                data64, sizeof(data64))) {
            init_file_result(
                &step_result, ESP_ERR_INVALID_SIZE);
            ret = ESP_ERR_INVALID_SIZE;
            goto verified_put_done;
        }
        crc32_hex(
            crc32_bytes(&data[offset], chunk_len),
            chunk_crc);
        const uint16_t chunk_id = next_file_request_id();
        command_len = snprintf(
            command, sizeof(command),
            "%s v=1 id=%u op=put_chunk off=%lu len=%lu "
            "data=%s crc=%s\n",
            D1L_RP2040_FILE_PREFIX, (unsigned)chunk_id,
            (unsigned long)offset, (unsigned long)chunk_len,
            data64, chunk_crc);
        if (command_len <= 0 ||
            (size_t)command_len >= sizeof(command)) {
            init_file_result(
                &step_result, ESP_ERR_INVALID_SIZE);
            ret = ESP_ERR_INVALID_SIZE;
            goto verified_put_done;
        }
        ret = send_file_command_while_locked(
            command, (size_t)command_len, chunk_id,
            "put_chunk", &step_result, timeout_ms);
        if (ret != ESP_OK &&
            verified_put_cleanup_confirmed(&step_result)) {
            session_maybe_active = false;
        }
        if (ret == ESP_OK) {
            ret = d1l_rp2040_file_reply_bind_put_chunk(
                &step_result, (uint32_t)offset, chunk_len);
        }
        if (ret != ESP_OK) {
            goto verified_put_done;
        }
        offset += chunk_len;
    }

    if (!verified_put_continue(
            should_continue, continue_context)) {
        note_verified_put_cancelled(&step_result);
        ret = step_result.last_error;
        goto verified_put_done;
    }
    const uint16_t end_id = next_file_request_id();
    command_len = snprintf(
        command, sizeof(command),
        "%s v=1 id=%u op=put_end\n",
        D1L_RP2040_FILE_PREFIX, (unsigned)end_id);
    if (command_len <= 0 ||
        (size_t)command_len >= sizeof(command)) {
        init_file_result(&step_result, ESP_ERR_INVALID_SIZE);
        ret = ESP_ERR_INVALID_SIZE;
        goto verified_put_done;
    }
    ret = send_file_command_while_locked(
        command, (size_t)command_len, end_id,
        "put_end", &step_result, timeout_ms);
    if (ret != ESP_OK &&
        verified_put_cleanup_confirmed(&step_result)) {
        session_maybe_active = false;
    }
    if (ret == ESP_OK) {
        ret = d1l_rp2040_file_reply_bind_put_end(
            &step_result, expected_size, expected_crc32);
    }
    if (ret == ESP_OK) {
        session_maybe_active = false;
    }

verified_put_done:
    if (ret != ESP_OK && session_maybe_active) {
        const bool cancelled = step_result.cancelled;
        d1l_rp2040_file_result_t abort_result = {0};
        const esp_err_t abort_ret =
            abort_verified_put_while_locked(
                &abort_result, timeout_ms);
        if (abort_ret == ESP_OK) {
            step_result.removed = abort_result.removed;
        } else {
            snprintf(step_result.note,
                     sizeof(step_result.note), "abort_failed");
        }
        step_result.cancelled = cancelled;
    }
    *out_result = step_result;
    out_result->last_error = ret;
    give_bridge_lock();
    return ret;
}

esp_err_t d1l_rp2040_bridge_file_append(const char *path,
                                        const uint8_t *data,
                                        size_t len,
                                        d1l_rp2040_file_result_t *out_result,
                                        uint32_t timeout_ms)
{
    if (!path || !out_result || !data || len == 0 || len > D1L_RP2040_FILE_CHUNK_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    char path64[D1L_RP2040_PATH64_MAX + 1U];
    char data64[D1L_RP2040_DATA64_MAX + 1U];
    char crc[9];
    if (!encode_path(path, path64, sizeof(path64)) ||
        !base64url_encode(data, len, data64, sizeof(data64))) {
        init_file_result(out_result, ESP_ERR_INVALID_ARG);
        snprintf(out_result->err, sizeof(out_result->err), "bad_path");
        snprintf(out_result->note, sizeof(out_result->note), "bad_path");
        return ESP_ERR_INVALID_ARG;
    }
    crc32_hex(crc32_bytes(data, len), crc);
    const uint16_t request_id = next_file_request_id();
    char command[D1L_RP2040_FILE_LINE_MAX + 2U];
    const int command_len = snprintf(command, sizeof(command),
                                     "%s v=1 id=%u op=append path=%s len=%lu data=%s crc=%s\n",
                                     D1L_RP2040_FILE_PREFIX, (unsigned)request_id, path64,
                                     (unsigned long)len, data64, crc);
    if (command_len <= 0 || (size_t)command_len >= sizeof(command)) {
        init_file_result(out_result, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t ret = send_file_command(
        command, (size_t)command_len, request_id, "append",
        NULL, 0, out_result, timeout_ms);
    return ret == ESP_OK ? d1l_rp2040_file_reply_bind_append(
                              out_result, len) : ret;
}

esp_err_t d1l_rp2040_bridge_file_delete(const char *path,
                                        d1l_rp2040_file_result_t *out_result,
                                        uint32_t timeout_ms)
{
    if (!path || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    char path64[D1L_RP2040_PATH64_MAX + 1U];
    if (!encode_path(path, path64, sizeof(path64))) {
        init_file_result(out_result, ESP_ERR_INVALID_ARG);
        snprintf(out_result->err, sizeof(out_result->err), "bad_path");
        snprintf(out_result->note, sizeof(out_result->note), "bad_path");
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t request_id = next_file_request_id();
    char command[D1L_RP2040_FILE_LINE_MAX + 2U];
    const int command_len = snprintf(command, sizeof(command),
                                     "%s v=1 id=%u op=delete path=%s\n",
                                     D1L_RP2040_FILE_PREFIX, (unsigned)request_id, path64);
    if (command_len <= 0 || (size_t)command_len >= sizeof(command)) {
        init_file_result(out_result, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    return send_file_command(command, (size_t)command_len, request_id, "delete",
                             NULL, 0, out_result, timeout_ms);
}

esp_err_t d1l_rp2040_bridge_file_rename(const char *from_path,
                                        const char *to_path,
                                        bool replace,
                                        d1l_rp2040_file_result_t *out_result,
                                        uint32_t timeout_ms)
{
    if (!from_path || !to_path || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    char from64[D1L_RP2040_PATH64_MAX + 1U];
    char to64[D1L_RP2040_PATH64_MAX + 1U];
    if (!encode_path(from_path, from64, sizeof(from64)) ||
        !encode_path(to_path, to64, sizeof(to64))) {
        init_file_result(out_result, ESP_ERR_INVALID_ARG);
        snprintf(out_result->err, sizeof(out_result->err), "bad_path");
        snprintf(out_result->note, sizeof(out_result->note), "bad_path");
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t request_id = next_file_request_id();
    char command[D1L_RP2040_FILE_LINE_MAX + 2U];
    const int command_len = snprintf(command, sizeof(command),
                                     "%s v=1 id=%u op=rename path=%s to=%s replace=%u\n",
                                     D1L_RP2040_FILE_PREFIX, (unsigned)request_id, from64, to64,
                                     replace ? 1U : 0U);
    if (command_len <= 0 || (size_t)command_len >= sizeof(command)) {
        init_file_result(out_result, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    return send_file_command(command, (size_t)command_len, request_id, "rename",
                             NULL, 0, out_result, timeout_ms);
}
