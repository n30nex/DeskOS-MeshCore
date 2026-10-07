#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "platform/time_display.h"

static void test_offsets_and_parser_are_bounded(void)
{
    assert(d1l_time_display_offset_valid(-720));
    assert(d1l_time_display_offset_valid(840));
    assert(!d1l_time_display_offset_valid(-721));
    assert(!d1l_time_display_offset_valid(841));

    int16_t offset = 99;
    assert(d1l_time_display_parse_timezone("UTC", &offset));
    assert(offset == 0);
    assert(d1l_time_display_parse_timezone("UTC+14:00", &offset));
    assert(offset == 840);
    assert(d1l_time_display_parse_timezone("UTC-12:00", &offset));
    assert(offset == -720);
    assert(d1l_time_display_parse_timezone("UTC+05:45", &offset));
    assert(offset == 345);
    assert(!d1l_time_display_parse_timezone("utc", &offset));
    assert(!d1l_time_display_parse_timezone("UTC+14:01", &offset));
    assert(!d1l_time_display_parse_timezone("UTC-12:01", &offset));
    assert(!d1l_time_display_parse_timezone("UTC+05:60", &offset));
    assert(!d1l_time_display_parse_timezone("UTC+05:45 ", &offset));
    assert(!d1l_time_display_parse_timezone(NULL, &offset));
    assert(!d1l_time_display_parse_timezone("UTC", NULL));
}

static void test_labels_are_explicit_fixed_offsets(void)
{
    char label[D1L_TIMEZONE_LABEL_LEN] = {0};
    assert(d1l_time_display_timezone_label(0, label, sizeof(label)));
    assert(strcmp(label, "UTC") == 0);
    assert(d1l_time_display_timezone_label(840, label, sizeof(label)));
    assert(strcmp(label, "UTC+14:00") == 0);
    assert(d1l_time_display_timezone_label(-720, label, sizeof(label)));
    assert(strcmp(label, "UTC-12:00") == 0);
    assert(!d1l_time_display_timezone_label(841, label, sizeof(label)));
    assert(label[0] == '\0');
    assert(!d1l_time_display_timezone_label(0, label, 3U));
    assert(label[0] == '\0');
}

static void test_clock_conversion_is_display_only_and_floor_modulo(void)
{
    char clock[D1L_TIME_DISPLAY_CLOCK_LEN] = {0};
    assert(d1l_time_display_format_clock(0, 0, false,
                                         clock, sizeof(clock)));
    assert(strcmp(clock, "00:00") == 0);
    assert(d1l_time_display_format_clock(0, 840, false,
                                         clock, sizeof(clock)));
    assert(strcmp(clock, "14:00") == 0);
    assert(d1l_time_display_format_clock(0, -720, false,
                                         clock, sizeof(clock)));
    assert(strcmp(clock, "12:00") == 0);
    assert(d1l_time_display_format_clock(-60, 0, false,
                                         clock, sizeof(clock)));
    assert(strcmp(clock, "23:59") == 0);
    assert(d1l_time_display_format_clock(3660, 0, true,
                                         clock, sizeof(clock)));
    assert(strcmp(clock, "~01:01") == 0);
    assert(!d1l_time_display_format_clock(INT64_MAX, 840, false,
                                          clock, sizeof(clock)));
    assert(clock[0] == '\0');
    assert(!d1l_time_display_format_clock(INT64_MIN, -720, false,
                                          clock, sizeof(clock)));
    assert(clock[0] == '\0');
}

static void test_daylight_saving_boundaries(void)
{
    assert(d1l_time_display_offset_at(INT64_C(1772953199), -300, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -300);
    assert(d1l_time_display_offset_at(INT64_C(1772953200), -300, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -240);
    assert(d1l_time_display_offset_at(INT64_C(1793512799), -300, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -240);
    assert(d1l_time_display_offset_at(INT64_C(1793512800), -300, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -300);
    assert(d1l_time_display_offset_at(INT64_C(1772947799), -210, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -210);
    assert(d1l_time_display_offset_at(INT64_C(1772947800), -210, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -150);
    assert(d1l_time_display_offset_at(INT64_C(1793507399), -210, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -150);
    assert(d1l_time_display_offset_at(INT64_C(1793507400), -210, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -210);
    assert(d1l_time_display_offset_at(INT64_C(1836457199), -300, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -300);
    assert(d1l_time_display_offset_at(INT64_C(1836457200), -300, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -240);
    assert(d1l_time_display_offset_at(INT64_C(1857016799), -300, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -240);
    assert(d1l_time_display_offset_at(INT64_C(1857016800), -300, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -300);
    assert(d1l_time_display_offset_at(INT64_C(1774745999), 60, D1L_DAYLIGHT_SAVING_EUROPE) == 60);
    assert(d1l_time_display_offset_at(INT64_C(1774746000), 60, D1L_DAYLIGHT_SAVING_EUROPE) == 120);
    assert(d1l_time_display_offset_at(INT64_C(1792889999), 60, D1L_DAYLIGHT_SAVING_EUROPE) == 120);
    assert(d1l_time_display_offset_at(INT64_C(1792890000), 60, D1L_DAYLIGHT_SAVING_EUROPE) == 60);
    assert(d1l_time_display_offset_at(INT64_C(1774745999), 0, D1L_DAYLIGHT_SAVING_EUROPE) == 0);
    assert(d1l_time_display_offset_at(INT64_C(1774746000), 0, D1L_DAYLIGHT_SAVING_EUROPE) == 60);
    assert(d1l_time_display_offset_at(INT64_C(1792889999), 0, D1L_DAYLIGHT_SAVING_EUROPE) == 60);
    assert(d1l_time_display_offset_at(INT64_C(1792890000), 0, D1L_DAYLIGHT_SAVING_EUROPE) == 0);
    assert(d1l_time_display_offset_at(INT64_MAX, -300, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -300);
    assert(d1l_time_display_offset_at(INT64_MIN, -300, D1L_DAYLIGHT_SAVING_NORTH_AMERICA) == -300);
    assert(d1l_time_display_offset_at(1780000000, 345, D1L_DAYLIGHT_SAVING_OFF) == 345);
    assert(d1l_time_display_offset_at(1780000000, 840, D1L_DAYLIGHT_SAVING_EUROPE) == 840);
    assert(d1l_time_display_offset_at(1780000000, -300, (d1l_daylight_saving_t)99) == -300);
}

int main(void)
{
    test_daylight_saving_boundaries();
    test_offsets_and_parser_are_bounded();
    test_labels_are_explicit_fixed_offsets();
    test_clock_conversion_is_display_only_and_floor_modulo();
    puts("native time display: ok");
    return 0;
}
