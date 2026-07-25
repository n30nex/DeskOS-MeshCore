#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define D1L_EVENT_LOG_CAPACITY 64U
#define D1L_EVENT_LOG_SOURCE_LEN 16U
#define D1L_EVENT_LOG_KIND_LEN 20U
#define D1L_EVENT_LOG_MESSAGE_LEN 80U

typedef enum {
    D1L_EVENT_LOG_LEVEL_ERROR = 0,
    D1L_EVENT_LOG_LEVEL_WARN,
    D1L_EVENT_LOG_LEVEL_INFO,
    D1L_EVENT_LOG_LEVEL_DEBUG,
} d1l_event_log_level_t;

typedef struct {
    uint32_t sequence;
    uint32_t uptime_ms;
    d1l_event_log_level_t level;
    char source[D1L_EVENT_LOG_SOURCE_LEN];
    char kind[D1L_EVENT_LOG_KIND_LEN];
    char message[D1L_EVENT_LOG_MESSAGE_LEN];
} d1l_event_log_entry_t;

typedef struct {
    uint32_t next_sequence;
    uint32_t total_written;
    uint32_t dropped_oldest;
    size_t count;
    size_t capacity;
    d1l_event_log_level_t runtime_level;
} d1l_event_log_status_t;

void d1l_event_log_init(void);
void d1l_event_log_append(d1l_event_log_level_t level,
                          const char *source,
                          const char *kind,
                          const char *message);
size_t d1l_event_log_copy_recent(d1l_event_log_entry_t *out_entries,
                                 size_t max_entries);
d1l_event_log_status_t d1l_event_log_status(void);
esp_err_t d1l_event_log_clear(void);
esp_err_t d1l_event_log_set_runtime_level(d1l_event_log_level_t level);
const char *d1l_event_log_level_name(d1l_event_log_level_t level);
bool d1l_event_log_level_from_name(const char *name,
                                   d1l_event_log_level_t *out_level);

#ifdef __cplusplus
}
#endif
