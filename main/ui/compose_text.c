#include "compose_text.h"

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"

/* UI-task owned. Never persisted, logged or included in support exports. */
static char s_clipboard[D1L_DRAFT_TEXT_BYTES] EXT_RAM_BSS_ATTR;

void d1l_compose_clipboard_clear(void)
{
    volatile unsigned char *p = (volatile unsigned char *)s_clipboard;
    for (size_t i = 0U; i < sizeof(s_clipboard); ++i) p[i] = 0U;
}

bool d1l_compose_clipboard_copy(const char *text)
{
    if (!text) return false;
    const char *end = memchr(text, '\0', sizeof(s_clipboard));
    if (!end || end == text || d1l_user_text_validate_display_span(
            (const uint8_t *)text, (size_t)(end - text)).result != D1L_USER_TEXT_OK) return false;
    const size_t length = (size_t)(end - text);
    memmove(s_clipboard, text, length);
    memset(s_clipboard + length, 0, sizeof(s_clipboard) - length);
    return true;
}

const char *d1l_compose_clipboard_text(void) { return s_clipboard; }

bool d1l_compose_text_fits(const char *draft, const char *addition)
{
    const d1l_user_text_info_t a = d1l_user_text_validate(draft);
    const d1l_user_text_info_t b = d1l_user_text_validate(addition);
    return (a.result == D1L_USER_TEXT_OK || a.result == D1L_USER_TEXT_EMPTY) &&
        b.result == D1L_USER_TEXT_OK && a.byte_count + b.byte_count <= D1L_USER_TEXT_MAX_BYTES;
}

static size_t prefix_length(const char *text, size_t limit)
{
    size_t length = strlen(text);
    if (length <= limit) return length;
    length = limit;
    while (length && ((unsigned char)text[length] & 0xc0U) == 0x80U) --length;
    return length;
}

bool d1l_compose_quote(char *out, size_t capacity, const char *author, const char *text)
{
    if (!out || capacity == 0U) return false;
    out[0] = '\0';
    if (!author || !author[0]) author = "Unknown";
    if (d1l_user_text_validate(author).result != D1L_USER_TEXT_OK ||
        d1l_user_text_validate(text).result != D1L_USER_TEXT_OK) return false;
    const size_t name_length = prefix_length(author, 24U);
    const size_t text_length = prefix_length(text, 48U);
    const int length = snprintf(out, capacity, "> %.*s: %.*s%s | ",
        (int)name_length, author, (int)text_length, text, text[text_length] ? "..." : "");
    if (length < 0 || (size_t)length >= capacity || (size_t)length > D1L_USER_TEXT_MAX_BYTES) {
        out[0] = '\0';
        return false;
    }
    return true;
}
