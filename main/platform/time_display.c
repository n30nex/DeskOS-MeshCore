#define _POSIX_C_SOURCE 200809L
#include "time_display.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

const char *d1l_daylight_saving_name(d1l_daylight_saving_t rule)
{
    switch (rule) {
    case D1L_DAYLIGHT_SAVING_OFF: return "Off";
    case D1L_DAYLIGHT_SAVING_NORTH_AMERICA: return "US / Canada";
    case D1L_DAYLIGHT_SAVING_EUROPE: return "Europe / UK";
    default: return "Unknown";
    }
}

int16_t d1l_time_display_offset_at(int64_t utc_epoch_sec,
                                  int16_t standard_offset_minutes,
                                  d1l_daylight_saving_t rule)
{
    if (!d1l_time_display_offset_valid(standard_offset_minutes)) return 0;
    if (rule == D1L_DAYLIGHT_SAVING_OFF ||
        rule > D1L_DAYLIGHT_SAVING_EUROPE || rule < 0 ||
        standard_offset_minutes > D1L_TIMEZONE_OFFSET_MINUTES_MAX - 60 ||
        utc_epoch_sec < INT64_C(1167609600) ||
        utc_epoch_sec > INT64_C(253402214399)) return standard_offset_minutes;

    /* North America changes at 02:00 local standard time in March and
     * 02:00 daylight time (01:00 standard) in November. Europe changes at
     * 01:00 UTC. Work in those reference clocks without process-global TZ. */
    const int64_t reference = utc_epoch_sec +
        (rule == D1L_DAYLIGHT_SAVING_NORTH_AMERICA ?
            (int64_t)standard_offset_minutes * 60 : 0);
    const time_t stamp = (time_t)reference;
    if ((int64_t)stamp != reference) return standard_offset_minutes;
    struct tm date;
#ifdef _WIN32
    if (gmtime_s(&date, &stamp) != 0) return standard_offset_minutes;
#else
    if (!gmtime_r(&stamp, &date)) return standard_offset_minutes;
#endif
    const int month = date.tm_mon + 1;
    const int end_month = rule == D1L_DAYLIGHT_SAVING_NORTH_AMERICA ? 11 : 10;
    bool summer = month > 3 && month < end_month;
    if (month == 3 || month == end_month) {
        const int first_sunday = 1 + (7 - (date.tm_wday - (date.tm_mday - 1) % 7 + 7) % 7) % 7;
        const int sunday = rule == D1L_DAYLIGHT_SAVING_NORTH_AMERICA ?
            first_sunday + (month == 3 ? 7 : 0) :
            first_sunday + ((31 - first_sunday) / 7) * 7;
        const int hour = rule == D1L_DAYLIGHT_SAVING_NORTH_AMERICA && month == 3 ? 2 : 1;
        const bool after = date.tm_mday > sunday ||
            (date.tm_mday == sunday && date.tm_hour >= hour);
        summer = month == 3 ? after : !after;
    }
    return (int16_t)(standard_offset_minutes + (summer ? 60 : 0));
}

bool d1l_time_display_offset_valid(int32_t offset_minutes)
{
    return offset_minutes >= D1L_TIMEZONE_OFFSET_MINUTES_MIN &&
           offset_minutes <= D1L_TIMEZONE_OFFSET_MINUTES_MAX;
}

bool d1l_time_display_parse_timezone(const char *text,
                                     int16_t *out_offset_minutes)
{
    if (!text || !out_offset_minutes) {
        return false;
    }
    if (strcmp(text, "UTC") == 0) {
        *out_offset_minutes = 0;
        return true;
    }
    if (strlen(text) != 9U || memcmp(text, "UTC", 3U) != 0 ||
        (text[3] != '+' && text[3] != '-') || text[6] != ':' ||
        !isdigit((unsigned char)text[4]) ||
        !isdigit((unsigned char)text[5]) ||
        !isdigit((unsigned char)text[7]) ||
        !isdigit((unsigned char)text[8])) {
        return false;
    }
    const int32_t hours = (int32_t)(text[4] - '0') * 10 +
                          (int32_t)(text[5] - '0');
    const int32_t minutes = (int32_t)(text[7] - '0') * 10 +
                            (int32_t)(text[8] - '0');
    if (minutes > 59) {
        return false;
    }
    int32_t offset = hours * 60 + minutes;
    if (text[3] == '-') {
        offset = -offset;
    }
    if (!d1l_time_display_offset_valid(offset)) {
        return false;
    }
    *out_offset_minutes = (int16_t)offset;
    return true;
}

bool d1l_time_display_timezone_label(int16_t offset_minutes,
                                     char *destination,
                                     size_t destination_size)
{
    if (!destination || destination_size < D1L_TIMEZONE_LABEL_LEN ||
        !d1l_time_display_offset_valid(offset_minutes)) {
        if (destination && destination_size > 0U) {
            destination[0] = '\0';
        }
        return false;
    }
    if (offset_minutes == 0) {
        (void)snprintf(destination, destination_size, "UTC");
        return true;
    }
    const int32_t magnitude = offset_minutes < 0 ?
        -(int32_t)offset_minutes : (int32_t)offset_minutes;
    (void)snprintf(destination, destination_size, "UTC%c%02ld:%02ld",
                   offset_minutes < 0 ? '-' : '+',
                   (long)(magnitude / 60), (long)(magnitude % 60));
    return true;
}

bool d1l_time_display_format_clock(int64_t utc_epoch_sec,
                                   int16_t offset_minutes,
                                   bool approximate,
                                   char *destination,
                                   size_t destination_size)
{
    if (!destination || destination_size < D1L_TIME_DISPLAY_CLOCK_LEN ||
        !d1l_time_display_offset_valid(offset_minutes)) {
        if (destination && destination_size > 0U) {
            destination[0] = '\0';
        }
        return false;
    }
    const int64_t delta = (int64_t)offset_minutes * INT64_C(60);
    if ((delta > 0 && utc_epoch_sec > INT64_MAX - delta) ||
        (delta < 0 && utc_epoch_sec < INT64_MIN - delta)) {
        destination[0] = '\0';
        return false;
    }
    const int64_t local_epoch = utc_epoch_sec + delta;
    int64_t second_of_day = local_epoch % INT64_C(86400);
    if (second_of_day < 0) {
        second_of_day += INT64_C(86400);
    }
    const int64_t hour = second_of_day / INT64_C(3600);
    const int64_t minute = (second_of_day % INT64_C(3600)) / INT64_C(60);
    (void)snprintf(destination, destination_size,
                   approximate ? "~%02ld:%02ld" : "%02ld:%02ld",
                   (long)hour, (long)minute);
    return true;
}
