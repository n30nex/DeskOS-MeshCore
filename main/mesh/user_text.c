#include "user_text.h"

#include <string.h>

static d1l_user_text_info_t result(d1l_user_text_result_t status,
                                   size_t byte_count,
                                   size_t character_count)
{
    const d1l_user_text_info_t info = {
        .result = status,
        .byte_count = byte_count,
        .character_count = character_count,
    };
    return info;
}

static bool continuation(uint8_t byte)
{
    return (byte & 0xc0U) == 0x80U;
}

static bool forbidden_control(uint32_t scalar)
{
    return scalar < 0x20U ||
           (scalar >= 0x7fU && scalar <= 0x9fU);
}

static d1l_user_text_info_t validate_span(const uint8_t *text,
                                        size_t byte_count, bool allow_empty,
                                        size_t maximum, bool multiline)
{
    if (!text) {
        return result(D1L_USER_TEXT_INVALID_UTF8, 0U, 0U);
    }
    if (byte_count == 0U) {
        return result(allow_empty ? D1L_USER_TEXT_OK : D1L_USER_TEXT_EMPTY,
                      0U, 0U);
    }
    if (byte_count > maximum) {
        return result(D1L_USER_TEXT_TOO_LONG, byte_count, 0U);
    }

    size_t index = 0U;
    size_t characters = 0U;
    while (index < byte_count) {
        const uint8_t first = text[index];
        uint32_t scalar = 0U;
        size_t width = 0U;

        if (first <= 0x7fU) {
            scalar = first;
            width = 1U;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            if (byte_count - index < 2U || !continuation(text[index + 1U])) {
                return result(D1L_USER_TEXT_INVALID_UTF8, byte_count, characters);
            }
            scalar = ((uint32_t)(first & 0x1fU) << 6U) |
                     (uint32_t)(text[index + 1U] & 0x3fU);
            width = 2U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            if (byte_count - index < 3U ||
                !continuation(text[index + 1U]) ||
                !continuation(text[index + 2U]) ||
                (first == 0xe0U && text[index + 1U] < 0xa0U) ||
                (first == 0xedU && text[index + 1U] > 0x9fU)) {
                return result(D1L_USER_TEXT_INVALID_UTF8, byte_count, characters);
            }
            scalar = ((uint32_t)(first & 0x0fU) << 12U) |
                     ((uint32_t)(text[index + 1U] & 0x3fU) << 6U) |
                     (uint32_t)(text[index + 2U] & 0x3fU);
            width = 3U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            if (byte_count - index < 4U ||
                !continuation(text[index + 1U]) ||
                !continuation(text[index + 2U]) ||
                !continuation(text[index + 3U]) ||
                (first == 0xf0U && text[index + 1U] < 0x90U) ||
                (first == 0xf4U && text[index + 1U] > 0x8fU)) {
                return result(D1L_USER_TEXT_INVALID_UTF8, byte_count, characters);
            }
            scalar = ((uint32_t)(first & 0x07U) << 18U) |
                     ((uint32_t)(text[index + 1U] & 0x3fU) << 12U) |
                     ((uint32_t)(text[index + 2U] & 0x3fU) << 6U) |
                     (uint32_t)(text[index + 3U] & 0x3fU);
            width = 4U;
        } else {
            return result(D1L_USER_TEXT_INVALID_UTF8, byte_count, characters);
        }

        if (forbidden_control(scalar) &&
            !(multiline && (scalar == '\n' || scalar == '\r' || scalar == '\t'))) {
            return result(D1L_USER_TEXT_CONTROL_CHARACTER, byte_count, characters);
        }
        index += width;
        characters++;
    }
    return result(D1L_USER_TEXT_OK, byte_count, characters);
}

d1l_user_text_info_t d1l_user_text_validate_span(const uint8_t *text,
                                              size_t byte_count,
                                              bool allow_empty)
{
    return validate_span(text, byte_count, allow_empty,
                         D1L_USER_TEXT_MAX_BYTES, false);
}

d1l_user_text_info_t d1l_user_text_validate_display_span(
    const uint8_t *text, size_t byte_count)
{
    return validate_span(text, byte_count, false, SIZE_MAX, true);
}

d1l_user_text_info_t d1l_user_text_validate(const char *text)
{
    if (!text) {
        return result(D1L_USER_TEXT_EMPTY, 0U, 0U);
    }
    size_t length = 0U;
    while (length <= D1L_USER_TEXT_MAX_BYTES && text[length] != '\0') {
        length++;
    }
    if (length > D1L_USER_TEXT_MAX_BYTES) {
        return result(D1L_USER_TEXT_TOO_LONG, length, 0U);
    }
    return d1l_user_text_validate_span((const uint8_t *)text, length, false);
}

d1l_user_text_info_t d1l_user_text_validate_bounded(const char *text,
                                                    size_t capacity,
                                                    bool allow_empty)
{
    if (!text || capacity == 0U) {
        return result(D1L_USER_TEXT_NOT_TERMINATED, 0U, 0U);
    }
    const char *terminator = (const char *)memchr(text, '\0', capacity);
    if (!terminator) {
        return result(D1L_USER_TEXT_NOT_TERMINATED, capacity, 0U);
    }
    const size_t length = (size_t)(terminator - text);
    return d1l_user_text_validate_span((const uint8_t *)text, length,
                                       allow_empty);
}

d1l_user_text_result_t d1l_user_text_copy(char *dest, size_t dest_size,
                                         const char *src)
{
    const d1l_user_text_info_t info = d1l_user_text_validate(src);
    if (!dest || dest_size == 0U || info.result != D1L_USER_TEXT_OK) {
        return info.result == D1L_USER_TEXT_OK ? D1L_USER_TEXT_TOO_LONG :
                                                info.result;
    }
    if (info.byte_count + 1U > dest_size) {
        return D1L_USER_TEXT_TOO_LONG;
    }
    memcpy(dest, src, info.byte_count + 1U);
    return D1L_USER_TEXT_OK;
}
