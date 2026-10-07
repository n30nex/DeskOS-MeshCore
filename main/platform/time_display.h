#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define D1L_TIMEZONE_SETTING_SCHEMA_VERSION 1U
#define D1L_TIMEZONE_OFFSET_MINUTES_MIN (-720)
#define D1L_TIMEZONE_OFFSET_MINUTES_MAX 840
#define D1L_TIMEZONE_LABEL_LEN 10U
#define D1L_TIME_DISPLAY_CLOCK_LEN 8U

typedef enum {
    D1L_DAYLIGHT_SAVING_OFF = 0,
    D1L_DAYLIGHT_SAVING_NORTH_AMERICA,
    D1L_DAYLIGHT_SAVING_EUROPE,
} d1l_daylight_saving_t;

const char *d1l_daylight_saving_name(d1l_daylight_saving_t rule);
/* The configured offset is standard (winter) time. No protocol clock changes. */
int16_t d1l_time_display_offset_at(int64_t utc_epoch_sec,
                                  int16_t standard_offset_minutes,
                                  d1l_daylight_saving_t rule);

bool d1l_time_display_offset_valid(int32_t offset_minutes);
bool d1l_time_display_parse_timezone(const char *text,
                                     int16_t *out_offset_minutes);
bool d1l_time_display_timezone_label(int16_t offset_minutes,
                                     char *destination,
                                     size_t destination_size);
bool d1l_time_display_format_clock(int64_t utc_epoch_sec,
                                   int16_t offset_minutes,
                                   bool approximate,
                                   char *destination,
                                   size_t destination_size);
