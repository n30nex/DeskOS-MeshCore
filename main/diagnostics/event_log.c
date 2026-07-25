#include "event_log.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_event_log_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_event_log_entry_t
    s_entries[D1L_EVENT_LOG_CAPACITY] EXT_RAM_BSS_ATTR;
static size_t s_head;
static size_t s_count;
static uint32_t s_next_sequence = 1U;
static uint32_t s_total_written;
static uint32_t s_dropped_oldest;
static d1l_event_log_level_t s_runtime_level = D1L_EVENT_LOG_LEVEL_INFO;

static uint32_t uptime_ms(void)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us <= 0 ? 0U : (uint32_t)((uint64_t)now_us / 1000ULL);
}

static bool level_valid(d1l_event_log_level_t level)
{
    return level >= D1L_EVENT_LOG_LEVEL_ERROR &&
           level <= D1L_EVENT_LOG_LEVEL_DEBUG;
}

const char *d1l_event_log_level_name(d1l_event_log_level_t level)
{
    switch (level) {
    case D1L_EVENT_LOG_LEVEL_ERROR:
        return "error";
    case D1L_EVENT_LOG_LEVEL_WARN:
        return "warn";
    case D1L_EVENT_LOG_LEVEL_INFO:
        return "info";
    case D1L_EVENT_LOG_LEVEL_DEBUG:
        return "debug";
    default:
        return "invalid";
    }
}

bool d1l_event_log_level_from_name(const char *name,
                                   d1l_event_log_level_t *out_level)
{
    if (!name || !out_level) {
        return false;
    }
    static const struct {
        const char *name;
        d1l_event_log_level_t level;
    } values[] = {
        {"error", D1L_EVENT_LOG_LEVEL_ERROR},
        {"warn", D1L_EVENT_LOG_LEVEL_WARN},
        {"info", D1L_EVENT_LOG_LEVEL_INFO},
        {"debug", D1L_EVENT_LOG_LEVEL_DEBUG},
    };
    for (size_t i = 0U; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (strcmp(name, values[i].name) == 0) {
            *out_level = values[i].level;
            return true;
        }
    }
    return false;
}

void d1l_event_log_init(void)
{
    portENTER_CRITICAL(&s_event_log_lock);
    memset(s_entries, 0, sizeof(s_entries));
    s_head = 0U;
    s_count = 0U;
    s_next_sequence = 1U;
    s_total_written = 0U;
    s_dropped_oldest = 0U;
    s_runtime_level = D1L_EVENT_LOG_LEVEL_INFO;
    portEXIT_CRITICAL(&s_event_log_lock);
}

void d1l_event_log_append(d1l_event_log_level_t level,
                          const char *source,
                          const char *kind,
                          const char *message)
{
    if (!level_valid(level) || !source || !kind || !message ||
        source[0] == '\0' || kind[0] == '\0') {
        return;
    }
    d1l_event_log_entry_t entry = {
        .uptime_ms = uptime_ms(),
        .level = level,
    };
    snprintf(entry.source, sizeof(entry.source), "%s", source);
    snprintf(entry.kind, sizeof(entry.kind), "%s", kind);
    snprintf(entry.message, sizeof(entry.message), "%s", message);

    portENTER_CRITICAL(&s_event_log_lock);
    entry.sequence = s_next_sequence++;
    if (s_next_sequence == 0U) {
        s_next_sequence = 1U;
    }
    s_entries[s_head] = entry;
    s_head = (s_head + 1U) % D1L_EVENT_LOG_CAPACITY;
    if (s_count < D1L_EVENT_LOG_CAPACITY) {
        s_count++;
    } else {
        s_dropped_oldest++;
    }
    s_total_written++;
    portEXIT_CRITICAL(&s_event_log_lock);
}

size_t d1l_event_log_copy_recent(d1l_event_log_entry_t *out_entries,
                                 size_t max_entries)
{
    if (!out_entries || max_entries == 0U) {
        return 0U;
    }
    portENTER_CRITICAL(&s_event_log_lock);
    const size_t copy_count = s_count < max_entries ? s_count : max_entries;
    const size_t oldest = (s_head + D1L_EVENT_LOG_CAPACITY - s_count) %
                          D1L_EVENT_LOG_CAPACITY;
    const size_t skip = s_count - copy_count;
    for (size_t i = 0U; i < copy_count; ++i) {
        out_entries[i] =
            s_entries[(oldest + skip + i) % D1L_EVENT_LOG_CAPACITY];
    }
    portEXIT_CRITICAL(&s_event_log_lock);
    return copy_count;
}

d1l_event_log_status_t d1l_event_log_status(void)
{
    d1l_event_log_status_t status = {
        .capacity = D1L_EVENT_LOG_CAPACITY,
    };
    portENTER_CRITICAL(&s_event_log_lock);
    status.next_sequence = s_next_sequence;
    status.total_written = s_total_written;
    status.dropped_oldest = s_dropped_oldest;
    status.count = s_count;
    status.runtime_level = s_runtime_level;
    portEXIT_CRITICAL(&s_event_log_lock);
    return status;
}

esp_err_t d1l_event_log_clear(void)
{
    portENTER_CRITICAL(&s_event_log_lock);
    memset(s_entries, 0, sizeof(s_entries));
    s_head = 0U;
    s_count = 0U;
    s_dropped_oldest = 0U;
    portEXIT_CRITICAL(&s_event_log_lock);
    d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "terminal", "cleared",
                         "local event history cleared");
    return ESP_OK;
}

esp_err_t d1l_event_log_set_runtime_level(d1l_event_log_level_t level)
{
    if (!level_valid(level)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_log_level_t esp_level =
        level == D1L_EVENT_LOG_LEVEL_ERROR ? ESP_LOG_ERROR :
        level == D1L_EVENT_LOG_LEVEL_WARN ? ESP_LOG_WARN :
        level == D1L_EVENT_LOG_LEVEL_INFO ? ESP_LOG_INFO : ESP_LOG_DEBUG;
    esp_log_level_set("*", esp_level);
    portENTER_CRITICAL(&s_event_log_lock);
    s_runtime_level = level;
    portEXIT_CRITICAL(&s_event_log_lock);
    d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "terminal", "level",
                         d1l_event_log_level_name(level));
    return ESP_OK;
}
