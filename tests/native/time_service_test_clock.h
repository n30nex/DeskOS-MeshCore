#ifndef D1L_TIME_SERVICE_TEST_CLOCK_H
#define D1L_TIME_SERVICE_TEST_CLOCK_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <sys/time.h>
#include <time.h>

time_t d1l_test_time(time_t *out_time);

#ifdef _WIN32
int d1l_test_settimeofday(const struct timeval *value,
                          const void *timezone_value);
#else
int d1l_test_settimeofday(const struct timeval *value,
                          const struct timezone *timezone_value);
#endif

/* Load the platform declarations above before redirecting production calls. */
#define time d1l_test_time
#define settimeofday d1l_test_settimeofday

#endif
