#include "console_output.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define CONSOLE_FRAME_LIMIT (256U * 1024U)
static char *s_frame;
static size_t s_used;
static size_t s_capacity;
static bool s_active;
static const char *s_error;

static void emit_frame(void)
{
    if (s_error) {
        /* A command may already have run; never imply that a missing response
         * makes a mutating command safe to repeat automatically. */
        fprintf(stdout, "{\"schema\":1,\"ok\":false,\"cmd\":\"console\","
            "\"code\":\"%s\",\"hint\":\"Reply unavailable; verify device state before retrying\"}\n", s_error);
    } else if (s_used) {
        (void)fwrite(s_frame, 1U, s_used, stdout);
    }
    fflush(stdout);
    if (s_frame) {
        volatile char *bytes = s_frame;
        for (size_t i = 0U; i < s_used; ++i) bytes[i] = 0;
        free(s_frame);
    }
    s_frame = NULL;
    s_used = s_capacity = 0U;
    s_active = false;
    s_error = NULL;
}

int d1l_console_printf(const char *format, ...)
{
    if (!format) return -1;
    if (!s_active && format[0] == '{') s_active = true;
    va_list args;
    va_start(args, format);
    if (!s_active) {
        const int result = vprintf(format, args);
        va_end(args);
        return result;
    }
    va_list count_args;
    va_copy(count_args, args);
    const int length = vsnprintf(NULL, 0U, format, count_args);
    va_end(count_args);
    if (length < 0) s_error = "OUTPUT_FAILED";
    if (!s_error && (size_t)length > CONSOLE_FRAME_LIMIT - s_used) {
        s_error = "OUTPUT_TOO_LARGE";
    }
    if (!s_error) {
        const size_t required = s_used + (size_t)length + 1U;
        if (required > s_capacity) {
            size_t capacity = s_capacity ? s_capacity : 256U;
            while (capacity < required) capacity *= 2U;
            if (capacity > CONSOLE_FRAME_LIMIT + 1U) capacity = CONSOLE_FRAME_LIMIT + 1U;
#ifdef ESP_PLATFORM
            char *grown = heap_caps_realloc(s_frame, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
            char *grown = realloc(s_frame, capacity);
#endif
            if (!grown) s_error = "OUTPUT_NO_MEMORY";
            else { s_frame = grown; s_capacity = capacity; }
        }
        if (!s_error) {
            (void)vsnprintf(s_frame + s_used, s_capacity - s_used, format, args);
            s_used += (size_t)length;
        }
    }
    va_end(args);
    const bool finished = (s_used && s_frame[s_used - 1U] == '\n') ||
        (s_error && strchr(format, '\n') != NULL);
    const int result = s_error ? -1 : length;
    if (finished) emit_frame();
    return result;
}

int d1l_console_putchar(int character)
{
    const unsigned char value = (unsigned char)character;
    const int result = value == '\n' ? d1l_console_printf("\n") :
                                      d1l_console_printf("%c", value);
    return result < 0 ? EOF : value;
}
