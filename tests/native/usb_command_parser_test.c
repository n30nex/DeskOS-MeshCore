#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "comms/usb_command_parser.h"

static void assert_wiped(const char *buffer, size_t capacity)
{
    for (size_t i = 0U; i < capacity; ++i) {
        assert(buffer[i] == '\0');
    }
}

static d1l_usb_command_admit_status_t admit_bytes(
    const uint8_t *input, size_t input_length, char *buffer, size_t capacity,
    d1l_usb_command_view_t *view)
{
    memset(buffer, 0xa5, capacity);
    const size_t copied = input_length < capacity ? input_length : capacity;
    if (input && copied > 0U) {
        memcpy(buffer, input, copied);
    }
    return d1l_usb_command_admit_in_place(
        buffer, input_length, capacity, view);
}

static void test_valid_and_explicit_length_contract(void)
{
    char buffer[256];
    d1l_usb_command_view_t view = {0};
    const uint8_t help[] = {'h', 'e', 'l', 'p'};
    assert(admit_bytes(help, sizeof(help), buffer, sizeof(buffer), &view) ==
           D1L_USB_COMMAND_ADMIT_OK);
    assert(view.text == buffer && view.length == sizeof(help));
    assert(buffer[view.length] == '\0');
    assert(strlen(buffer) == view.length);
    assert(d1l_usb_command_equals(
        &view, "help", sizeof("help") - 1U));
    assert(!d1l_usb_command_equals(
        &view, "helpx", sizeof("helpx") - 1U));

    const uint8_t trailing_spaces[] = {'h', 'e', 'l', 'p', ' ', ' ', ' '};
    assert(admit_bytes(trailing_spaces, sizeof(trailing_spaces), buffer,
                       sizeof(buffer), &view) == D1L_USB_COMMAND_ADMIT_OK);
    assert(view.length == 4U && strcmp(buffer, "help") == 0);
    for (size_t i = view.length + 1U; i < sizeof(buffer); ++i) {
        assert(buffer[i] == '\0');
    }

    const uint8_t utf8_text[] = {
        'm', 'e', 's', 'h', ' ', 's', 'e', 'n', 'd', ' ', 'p', 'u', 'b',
        'l', 'i', 'c', ' ', 0xc3U, 0xa9U,
    };
    assert(admit_bytes(utf8_text, sizeof(utf8_text), buffer,
                       sizeof(buffer), &view) == D1L_USB_COMMAND_ADMIT_OK);
    assert(view.length == sizeof(utf8_text));
    assert(d1l_usb_command_has_argument(
        &view, "mesh send public ", sizeof("mesh send public ") - 1U));
}

static void test_exact_release_command_boundaries(void)
{
    char buffer[256];
    d1l_usb_command_view_t view = {0};
    const char *const exact_commands[] = {
        "nodes clear",
        "packets clear",
        "ui scroll-probe storage_card",
        "ui scroll-probe storage_data",
    };
    for (size_t i = 0U;
         i < sizeof(exact_commands) / sizeof(exact_commands[0]); ++i) {
        const char *literal = exact_commands[i];
        const size_t length = strlen(literal);
        assert(admit_bytes(
                   (const uint8_t *)literal, length, buffer, sizeof(buffer),
                   &view) == D1L_USB_COMMAND_ADMIT_OK);
        assert(d1l_usb_command_equals(&view, literal, length));

        char suffixed[64] = {0};
        assert(length + 2U < sizeof(suffixed));
        memcpy(suffixed, literal, length);
        memcpy(suffixed + length, " x", 3U);
        assert(admit_bytes(
                   (const uint8_t *)suffixed, length + 2U, buffer,
                   sizeof(buffer), &view) == D1L_USB_COMMAND_ADMIT_OK);
        assert(!d1l_usb_command_equals(&view, literal, length));
    }

    const char *const scroll_aliases[] = {
        "storage-card", "sd_card", "storage-data", "wi-fi", "wifi",
        "contact-route", "mesh-roles", "mesh_rooms", "mesh-repeaters",
        "map", "map-options", "map_options_sheet", "map-options-page",
        "map-menu", "map-location", "map_location_sheet",
        "map-location-page", "map-cache", "map_cache_page", "tile-cache",
    };
    for (size_t i = 0U;
         i < sizeof(scroll_aliases) / sizeof(scroll_aliases[0]); ++i) {
        char command[96] = "ui scroll-probe ";
        const size_t prefix_length = strlen(command);
        const size_t alias_length = strlen(scroll_aliases[i]);
        assert(prefix_length + alias_length + 1U < sizeof(command));
        memcpy(command + prefix_length, scroll_aliases[i],
               alias_length + 1U);
        assert(admit_bytes(
                   (const uint8_t *)command, strlen(command), buffer,
                   sizeof(buffer), &view) == D1L_USB_COMMAND_ADMIT_OK);
        assert(d1l_usb_command_has_argument(
            &view, "ui scroll-probe ",
            sizeof("ui scroll-probe ") - 1U));
    }

    const char *const compose_aliases[] = {
        "map-location", "map_location", "wifi", "wifi-ssid",
        "wifi_password", "wifi-pass",
    };
    for (size_t i = 0U;
         i < sizeof(compose_aliases) / sizeof(compose_aliases[0]); ++i) {
        char command[96] = "ui compose-probe ";
        const size_t prefix_length = strlen(command);
        const size_t alias_length = strlen(compose_aliases[i]);
        assert(prefix_length + alias_length + 1U < sizeof(command));
        memcpy(command + prefix_length, compose_aliases[i],
               alias_length + 1U);
        assert(admit_bytes(
                   (const uint8_t *)command, strlen(command), buffer,
                   sizeof(buffer), &view) == D1L_USB_COMMAND_ADMIT_OK);
        assert(d1l_usb_command_has_argument(
            &view, "ui compose-probe ",
            sizeof("ui compose-probe ") - 1U));
    }
}

static void test_hidden_and_control_bytes_fail_closed(void)
{
    char buffer[256];
    d1l_usb_command_view_t view = {.text = (const char *)1, .length = 99U};
    const uint8_t hidden_reboot[] = {
        'h', 'e', 'l', 'p', 0U, 'r', 'e', 'b', 'o', 'o', 't',
    };
    assert(admit_bytes(hidden_reboot, sizeof(hidden_reboot), buffer,
                       sizeof(buffer), &view) ==
           D1L_USB_COMMAND_ADMIT_EMBEDDED_NUL);
    assert(view.text == NULL && view.length == 0U);
    assert_wiped(buffer, sizeof(buffer));

    const uint8_t nul_at_end[] = {'r', 'e', 'b', 'o', 'o', 't', 0U};
    assert(admit_bytes(nul_at_end, sizeof(nul_at_end), buffer,
                       sizeof(buffer), &view) ==
           D1L_USB_COMMAND_ADMIT_EMBEDDED_NUL);
    assert_wiped(buffer, sizeof(buffer));

    const uint8_t controls[] = {0x01U, '\t', '\n', '\r', 0x1bU, 0x7fU};
    for (size_t i = 0U; i < sizeof(controls); ++i) {
        const uint8_t input[] = {'h', 'e', 'l', 'p', controls[i], 'x'};
        assert(admit_bytes(input, sizeof(input), buffer, sizeof(buffer),
                           &view) == D1L_USB_COMMAND_ADMIT_CONTROL_BYTE);
        assert_wiped(buffer, sizeof(buffer));
    }
}

static void test_empty_truncated_prefix_and_oversize_contract(void)
{
    char buffer[256];
    d1l_usb_command_view_t view = {0};
    const uint8_t spaces[] = {' ', ' ', ' '};
    assert(admit_bytes(spaces, sizeof(spaces), buffer, sizeof(buffer), &view) ==
           D1L_USB_COMMAND_ADMIT_EMPTY);
    assert_wiped(buffer, sizeof(buffer));

    const uint8_t truncated[] = {'m', 'e', 's', 'h', ' ', 's', 'e', 'n', 'd',
                                 ' ', 'p', 'u', 'b', 'l', 'i', 'c', ' '};
    assert(admit_bytes(truncated, sizeof(truncated), buffer, sizeof(buffer),
                       &view) == D1L_USB_COMMAND_ADMIT_OK);
    assert(!d1l_usb_command_has_argument(
        &view, "mesh send public ", sizeof("mesh send public ") - 1U));

    const uint8_t suffix_smuggle[] = {
        'r', 'e', 'b', 'o', 'o', 't', 'X',
    };
    assert(admit_bytes(suffix_smuggle, sizeof(suffix_smuggle), buffer,
                       sizeof(buffer), &view) == D1L_USB_COMMAND_ADMIT_OK);
    assert(!d1l_usb_command_equals(
        &view, "reboot", sizeof("reboot") - 1U));
    assert(!d1l_usb_command_token_or_argument(
        &view, "reboot", sizeof("reboot") - 1U));

    const uint8_t token_exact[] = {'m', 'e', 's', 's', 'a', 'g', 'e', 's',
                                   ' ', 'p', 'u', 'b', 'l', 'i', 'c'};
    assert(admit_bytes(token_exact, sizeof(token_exact), buffer,
                       sizeof(buffer), &view) == D1L_USB_COMMAND_ADMIT_OK);
    assert(d1l_usb_command_token_or_argument(
        &view, "messages public", sizeof("messages public") - 1U));
    const uint8_t token_arg[] = {
        'm', 'e', 's', 's', 'a', 'g', 'e', 's', ' ', 'p', 'u', 'b', 'l',
        'i', 'c', ' ', 'o', 'f', 'f', 's', 'e', 't', ' ', '8',
    };
    assert(admit_bytes(token_arg, sizeof(token_arg), buffer,
                       sizeof(buffer), &view) == D1L_USB_COMMAND_ADMIT_OK);
    assert(d1l_usb_command_token_or_argument(
        &view, "messages public", sizeof("messages public") - 1U));

    uint8_t maximum[D1L_USB_COMMAND_MAX_BYTES];
    memset(maximum, 'x', sizeof(maximum));
    assert(admit_bytes(maximum, sizeof(maximum), buffer, sizeof(buffer),
                       &view) == D1L_USB_COMMAND_ADMIT_OK);
    assert(view.length == D1L_USB_COMMAND_MAX_BYTES);
    assert(buffer[D1L_USB_COMMAND_MAX_BYTES] == '\0');

    uint8_t oversize[D1L_USB_COMMAND_MAX_BYTES + 1U];
    memset(oversize, 'x', sizeof(oversize));
    assert(admit_bytes(oversize, sizeof(oversize), buffer, sizeof(buffer),
                       &view) == D1L_USB_COMMAND_ADMIT_TOO_LONG);
    assert_wiped(buffer, sizeof(buffer));
}

static void test_numeric_suffixes_fail_closed(void)
{
    int integer = 91;
    assert(d1l_usb_command_parse_int_exact("7", &integer));
    assert(integer == 7);
    assert(d1l_usb_command_parse_int_exact("-9", &integer));
    assert(integer == -9);
    assert(d1l_usb_command_parse_int_exact(" 12", &integer));
    assert(integer == 12);
    assert(!d1l_usb_command_parse_int_exact("7x", &integer));
    assert(!d1l_usb_command_parse_int_exact("7 ", &integer));
    assert(!d1l_usb_command_parse_int_exact("", &integer));
    assert(!d1l_usb_command_parse_int_exact("999999999999999999999", &integer));

    double real = 0.0;
    assert(d1l_usb_command_parse_double_exact("902.5", &real));
    assert(real == 902.5);
    assert(!d1l_usb_command_parse_double_exact("902.5x", &real));
    assert(!d1l_usb_command_parse_double_exact("902.5 ", &real));
    assert(!d1l_usb_command_parse_double_exact("nan", &real));
    assert(!d1l_usb_command_parse_double_exact("inf", &real));
    assert(!d1l_usb_command_parse_double_exact("1e9999", &real));
}

static uint32_t next_random(uint32_t *state)
{
    *state = (*state * 1664525U) + 1013904223U;
    return *state;
}

static void test_deterministic_fuzz_contract(void)
{
    char buffer[256];
    uint8_t input[320];
    uint32_t state = 13746277U;
    size_t accepted = 0U;
    size_t rejected = 0U;

    for (size_t run = 0U; run < 100000U; ++run) {
        const size_t length = next_random(&state) % (sizeof(input) + 1U);
        for (size_t i = 0U; i < length; ++i) {
            input[i] = (uint8_t)(next_random(&state) >> 24U);
        }
        d1l_usb_command_view_t view = {
            .text = (const char *)1,
            .length = SIZE_MAX,
        };
        const d1l_usb_command_admit_status_t status = admit_bytes(
            input, length, buffer, sizeof(buffer), &view);
        if (status == D1L_USB_COMMAND_ADMIT_OK) {
            accepted++;
            assert(view.text == buffer);
            assert(view.length > 0U && view.length <= length);
            assert(view.length <= D1L_USB_COMMAND_MAX_BYTES);
            assert(buffer[view.length] == '\0');
            assert(strlen(buffer) == view.length);
            for (size_t i = 0U; i < view.length; ++i) {
                const uint8_t byte = (uint8_t)buffer[i];
                assert(byte >= 0x20U && byte != 0x7fU);
            }
        } else {
            rejected++;
            assert(view.text == NULL && view.length == 0U);
            assert_wiped(buffer, sizeof(buffer));
        }
    }
    assert(accepted > 0U && rejected > 0U);
}

int main(void)
{
    test_valid_and_explicit_length_contract();
    test_exact_release_command_boundaries();
    test_hidden_and_control_bytes_fail_closed();
    test_empty_truncated_prefix_and_oversize_contract();
    test_numeric_suffixes_fail_closed();
    test_deterministic_fuzz_contract();
    puts("native USB command parser: ok (100000 deterministic cases)");
    return 0;
}
