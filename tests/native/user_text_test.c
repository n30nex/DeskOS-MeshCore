#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mesh/user_text.h"

static void expect_span(const uint8_t *text, size_t length,
                        d1l_user_text_result_t expected)
{
    const d1l_user_text_info_t info =
        d1l_user_text_validate_span(text, length, false);
    assert(info.result == expected);
}

int main(void)
{
    char exact_ascii[D1L_USER_TEXT_MAX_BYTES + 1U];
    memset(exact_ascii, 'a', D1L_USER_TEXT_MAX_BYTES);
    exact_ascii[D1L_USER_TEXT_MAX_BYTES] = '\0';
    d1l_user_text_info_t info = d1l_user_text_validate(exact_ascii);
    assert(info.result == D1L_USER_TEXT_OK);
    assert(info.byte_count == 138U);
    assert(info.character_count == 138U);

    uint8_t exact_euro[D1L_USER_TEXT_MAX_BYTES];
    for (size_t i = 0U; i < sizeof(exact_euro); i += 3U) {
        exact_euro[i] = 0xe2U;
        exact_euro[i + 1U] = 0x82U;
        exact_euro[i + 2U] = 0xacU;
    }
    info = d1l_user_text_validate_span(exact_euro, sizeof(exact_euro), false);
    assert(info.result == D1L_USER_TEXT_OK);
    assert(info.byte_count == 138U);
    assert(info.character_count == 46U);

    const char mixed[] = {
        'C', 'a', 'f', (char)0xc3, (char)0xa9, ' ',
        (char)0xe6, (char)0x9d, (char)0xb1,
        (char)0xe4, (char)0xba, (char)0xac, ' ',
        (char)0xf0, (char)0x9f, (char)0x98, (char)0x80, ' ',
        '"', 'x', '\\', 'y', '"', '\0'
    };
    info = d1l_user_text_validate(mixed);
    assert(info.result == D1L_USER_TEXT_OK);
    assert(info.byte_count == sizeof(mixed) - 1U);
    assert(info.character_count == 15U);
    char copied[D1L_USER_TEXT_MAX_BYTES + 1U] = {0};
    assert(d1l_user_text_copy(copied, sizeof(copied), mixed) ==
           D1L_USER_TEXT_OK);
    assert(memcmp(copied, mixed, sizeof(mixed)) == 0);

    char too_long[140U];
    memset(too_long, 'b', sizeof(too_long) - 1U);
    too_long[sizeof(too_long) - 1U] = '\0';
    assert(d1l_user_text_validate(too_long).result == D1L_USER_TEXT_TOO_LONG);

    uint8_t emoji_140[140U];
    for (size_t i = 0U; i < sizeof(emoji_140); i += 4U) {
        emoji_140[i] = 0xf0U;
        emoji_140[i + 1U] = 0x9fU;
        emoji_140[i + 2U] = 0x98U;
        emoji_140[i + 3U] = 0x80U;
    }
    expect_span(emoji_140, sizeof(emoji_140), D1L_USER_TEXT_TOO_LONG);

    const uint8_t stray[] = {0x80U};
    const uint8_t truncated[] = {0xe2U, 0x82U};
    const uint8_t overlong[] = {0xc0U, 0xafU};
    const uint8_t surrogate[] = {0xedU, 0xa0U, 0x80U};
    const uint8_t above_unicode[] = {0xf4U, 0x90U, 0x80U, 0x80U};
    const uint8_t embedded_nul[] = {'o', 'k', 0U, 'x'};
    const uint8_t c0_control[] = {'a', '\n'};
    const uint8_t c1_control[] = {0xc2U, 0x80U};
    expect_span(stray, sizeof(stray), D1L_USER_TEXT_INVALID_UTF8);
    expect_span(truncated, sizeof(truncated), D1L_USER_TEXT_INVALID_UTF8);
    expect_span(overlong, sizeof(overlong), D1L_USER_TEXT_INVALID_UTF8);
    expect_span(surrogate, sizeof(surrogate), D1L_USER_TEXT_INVALID_UTF8);
    expect_span(above_unicode, sizeof(above_unicode), D1L_USER_TEXT_INVALID_UTF8);
    expect_span(embedded_nul, sizeof(embedded_nul), D1L_USER_TEXT_CONTROL_CHARACTER);
    expect_span(c0_control, sizeof(c0_control), D1L_USER_TEXT_CONTROL_CHARACTER);
    expect_span(c1_control, sizeof(c1_control), D1L_USER_TEXT_CONTROL_CHARACTER);

    assert(d1l_user_text_validate("").result == D1L_USER_TEXT_EMPTY);
    assert(d1l_user_text_validate_span((const uint8_t *)"", 0U, true).result ==
           D1L_USER_TEXT_OK);
    const char unterminated[] = {'n', 'o'};
    assert(d1l_user_text_validate_bounded(unterminated, sizeof(unterminated),
                                          false).result ==
           D1L_USER_TEXT_NOT_TERMINATED);

    const uint8_t display[] = "Hespeler \xf0\x9f\x92\x8e\r\n\tready";
    assert(d1l_user_text_validate_display_span(display, sizeof(display)-1U).result == D1L_USER_TEXT_OK);
    assert(d1l_user_text_validate_display_span(truncated, sizeof(truncated)).result == D1L_USER_TEXT_INVALID_UTF8);
    assert(d1l_user_text_validate_display_span(embedded_nul, sizeof(embedded_nul)).result == D1L_USER_TEXT_CONTROL_CHARACTER);
    assert(d1l_user_text_validate_display_span(c1_control, sizeof(c1_control)).result == D1L_USER_TEXT_CONTROL_CHARACTER);
    uint8_t long_display[160];
    memset(long_display, 'a', sizeof(long_display));
    assert(d1l_user_text_validate_display_span(long_display, sizeof(long_display)).result == D1L_USER_TEXT_OK);
    puts("native user text: ok");
    return 0;
}
