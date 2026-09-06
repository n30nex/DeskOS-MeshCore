#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define D1L_USER_TEXT_MAX_BYTES 138U

typedef enum {
    D1L_USER_TEXT_OK = 0,
    D1L_USER_TEXT_EMPTY,
    D1L_USER_TEXT_TOO_LONG,
    D1L_USER_TEXT_NOT_TERMINATED,
    D1L_USER_TEXT_INVALID_UTF8,
    D1L_USER_TEXT_CONTROL_CHARACTER,
} d1l_user_text_result_t;

typedef struct {
    d1l_user_text_result_t result;
    size_t byte_count;
    size_t character_count;
} d1l_user_text_info_t;

/* Validates a NUL-terminated user message without reading beyond the 139-byte
 * wire/storage envelope.  byte_count is the encoded UTF-8 length while
 * character_count is the number of decoded Unicode scalar values. */
d1l_user_text_info_t d1l_user_text_validate(const char *text);

/* Validates an exact byte span.  Embedded NUL is rejected as a control
 * character, which makes this suitable for authenticated plaintext before it
 * is admitted to stores or allowed to trigger an ACK. */
d1l_user_text_info_t d1l_user_text_validate_span(const uint8_t *text,
                                                 size_t byte_count,
                                                 bool allow_empty);

/* Validates a caller-bounded display span, including UTF-8 and layout
 * whitespace (tab/CR/LF). NUL, other controls, and malformed UTF-8 stay invalid. */
d1l_user_text_info_t d1l_user_text_validate_display_span(
    const uint8_t *text, size_t byte_count);

/* Validates a fixed retained field and requires a terminator inside capacity. */
d1l_user_text_info_t d1l_user_text_validate_bounded(const char *text,
                                                    size_t capacity,
                                                    bool allow_empty);

/* Strictly validates and copies one complete user message without truncation. */
d1l_user_text_result_t d1l_user_text_copy(char *dest, size_t dest_size,
                                         const char *src);
