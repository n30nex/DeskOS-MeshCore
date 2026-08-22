#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "mesh/advert_data.h"

#define LOCATION_MASK 0x10U
#define FEATURE1_MASK 0x20U
#define FEATURE2_MASK 0x40U
#define NAME_MASK 0x80U

static void put_i32_le(uint8_t *dst, int32_t value)
{
    const uint32_t raw = (uint32_t)value;
    dst[0] = (uint8_t)raw;
    dst[1] = (uint8_t)(raw >> 8);
    dst[2] = (uint8_t)(raw >> 16);
    dst[3] = (uint8_t)(raw >> 24);
}

static size_t append_optional_fields(uint8_t flags, uint8_t *data,
                                     int32_t lat_e6, int32_t lon_e6)
{
    size_t len = 0U;
    data[len++] = flags;
    if ((flags & LOCATION_MASK) != 0U) {
        put_i32_le(&data[len], lat_e6);
        put_i32_le(&data[len + 4U], lon_e6);
        len += 8U;
    }
    if ((flags & FEATURE1_MASK) != 0U) {
        data[len++] = 0x12U;
        data[len++] = 0x34U;
    }
    if ((flags & FEATURE2_MASK) != 0U) {
        data[len++] = 0x56U;
        data[len++] = 0x78U;
    }
    return len;
}

static d1l_advert_data_t parse_ok(const uint8_t *data, size_t len)
{
    d1l_advert_data_t parsed;
    memset(&parsed, 0xa5, sizeof(parsed));
    assert(d1l_advert_data_parse(data, len, &parsed));
    return parsed;
}

static void assert_rejected_canonically(const uint8_t *data, size_t len)
{
    d1l_advert_data_t parsed;
    memset(&parsed, 0xa5, sizeof(parsed));
    assert(!d1l_advert_data_parse(data, len, &parsed));
    assert(parsed.type_code == 'N');
    assert(parsed.name[0] == '\0');
    assert(!parsed.location_valid);
    assert(parsed.lat_e6 == 0);
    assert(parsed.lon_e6 == 0);
}

static void test_exact_cambridge_vector(void)
{
    const uint8_t data[] = {
        0x92U,
        0x89U, 0xa8U, 0x96U, 0x02U,
        0xc2U, 0x77U, 0x36U, 0xfbU,
        'T', 'o', 'd', 'd', 'm', 'a', 's',
    };
    const d1l_advert_data_t parsed = parse_ok(data, sizeof(data));
    assert(parsed.type_code == 'P');
    assert(parsed.location_valid);
    assert(parsed.lat_e6 == 43427977);
    assert(parsed.lon_e6 == -80316478);
    assert(strcmp(parsed.name, "Toddmas") == 0);
}

static void assert_location_result(int32_t lat_e6, int32_t lon_e6, bool valid)
{
    uint8_t data[9U] = {0};
    const size_t len = append_optional_fields(0x11U, data, lat_e6, lon_e6);
    d1l_advert_data_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    assert(d1l_advert_data_parse(data, len, &parsed) == valid);
    if (valid) {
        assert(parsed.type_code == 'C');
        assert(parsed.location_valid);
        assert(parsed.lat_e6 == lat_e6);
        assert(parsed.lon_e6 == lon_e6);
    }
}

static void test_signed_and_boundary_coordinates(void)
{
    assert_location_result(-43427977, 80316478, true);
    assert_location_result(-1, -1, true);
    assert_location_result(-90000000, -180000000, true);
    assert_location_result(90000000, 180000000, true);
    assert_location_result(90000001, 0, false);
    assert_location_result(-90000001, 0, false);
    assert_location_result(0, 180000001, false);
    assert_location_result(0, -180000001, false);
    assert_location_result(INT32_MAX, 0, false);
    assert_location_result(INT32_MIN, 0, false);
}

static uint8_t optional_flags(unsigned combination)
{
    uint8_t flags = 0x01U;
    if ((combination & 1U) != 0U) {
        flags |= LOCATION_MASK;
    }
    if ((combination & 2U) != 0U) {
        flags |= FEATURE1_MASK;
    }
    if ((combination & 4U) != 0U) {
        flags |= FEATURE2_MASK;
    }
    return flags;
}

static void test_every_optional_field_truncation_boundary(void)
{
    d1l_advert_data_t parsed;
    assert(d1l_advert_data_parse(NULL, 0U, &parsed));

    for (unsigned combination = 1U; combination < 8U; ++combination) {
        uint8_t data[13U] = {0};
        const size_t full_len = append_optional_fields(
            optional_flags(combination), data, 43427977, -80316478);
        for (size_t truncated_len = 1U; truncated_len < full_len; ++truncated_len) {
            assert_rejected_canonically(data, truncated_len);
        }
        assert(d1l_advert_data_parse(data, full_len, &parsed));
    }
}

static void test_every_optional_field_combination(void)
{
    for (unsigned combination = 0U; combination < 8U; ++combination) {
        uint8_t data[D1L_ADVERT_DATA_MAX_LEN] = {0};
        const uint8_t flags = (uint8_t)(optional_flags(combination) | NAME_MASK);
        size_t len = append_optional_fields(flags, data, 43427977, -80316478);
        memcpy(&data[len], "Node", 4U);
        len += 4U;

        const d1l_advert_data_t parsed = parse_ok(data, len);
        assert(parsed.type_code == 'C');
        assert(parsed.location_valid == ((combination & 1U) != 0U));
        assert(strcmp(parsed.name, "Node") == 0);

        data[0] &= (uint8_t)~NAME_MASK;
        const size_t without_name_len = len - 4U;
        const d1l_advert_data_t without_name = parse_ok(data, without_name_len);
        assert(without_name.name[0] == '\0');
    }

    const uint8_t no_name_with_trailing_data[] = {0x01U, 'i', 'g', 'n', 'o', 'r', 'e'};
    const d1l_advert_data_t trailing =
        parse_ok(no_name_with_trailing_data, sizeof(no_name_with_trailing_data));
    assert(trailing.type_code == 'C');
    assert(trailing.name[0] == '\0');
}

static void test_null_oversize_and_maximum_length(void)
{
    uint8_t maximum[D1L_ADVERT_DATA_MAX_LEN];
    memset(maximum, 'A', sizeof(maximum));
    maximum[0] = 0x81U;

    d1l_advert_data_t parsed = parse_ok(maximum, sizeof(maximum));
    assert(strlen(parsed.name) == D1L_ADVERT_DATA_NAME_LEN - 1U);
    assert(parsed.name[D1L_ADVERT_DATA_NAME_LEN - 1U] == '\0');

    uint8_t oversize[D1L_ADVERT_DATA_MAX_LEN + 1U] = {0};
    assert_rejected_canonically(oversize, sizeof(oversize));

    memset(&parsed, 0xa5, sizeof(parsed));
    assert(d1l_advert_data_parse(NULL, 0U, &parsed));
    assert(parsed.type_code == 'N');
    assert(parsed.name[0] == '\0');
    assert(!parsed.location_valid);

    assert_rejected_canonically(NULL, 1U);
    assert(!d1l_advert_data_parse(maximum, sizeof(maximum), NULL));
    assert(!d1l_advert_data_parse(NULL, 0U, NULL));
}

static void test_name_sanitization_and_type_mapping(void)
{
    const char expected_types[16U] = {
        'N', 'C', 'P', 'R', 'S', 'N', 'N', 'N',
        'N', 'N', 'N', 'N', 'N', 'N', 'N', 'N',
    };
    for (uint8_t type = 0U; type < 16U; ++type) {
        const uint8_t data[] = {type};
        const d1l_advert_data_t parsed = parse_ok(data, sizeof(data));
        assert(parsed.type_code == expected_types[type]);
    }

    const uint8_t dirty_name[] = {
        0x84U, 'A', 0x1fU, 0x20U, 0x7eU, 0x7fU, '"', '\\', 'B',
    };
    const d1l_advert_data_t sanitized = parse_ok(dirty_name, sizeof(dirty_name));
    assert(sanitized.type_code == 'S');
    assert(strcmp(sanitized.name, "A_ ~___B") == 0);

    const uint8_t padded_name[] = {
        0x82U, 'Y', 'K', 'F', ' ', '-', ' ', 'H', 'i', 'l', 'l', 't', 'o', 'p',
        0U, 0U, 0U, 0U,
    };
    const d1l_advert_data_t padded = parse_ok(padded_name, sizeof(padded_name));
    assert(strcmp(padded.name, "YKF - Hilltop") == 0);

    const uint8_t meaningful_underscore[] = {
        0x81U, 'K', 'i', 'n', 'z', 'e', '_', 'S', 'o', 'l', 'a', 'r',
    };
    const d1l_advert_data_t underscore = parse_ok(
        meaningful_underscore, sizeof(meaningful_underscore));
    assert(strcmp(underscore.name, "Kinze_Solar") == 0);

    const uint8_t fuzz_regression[] = {
        0x84U, 'A', 0x1fU, 0x84U, 0xafU, 0xccU, 0xc0U, 0x8aU, 0xe5U,
        0x7fU, 0U, 0U, ' ', '~', 0x7fU, '"', '\\', 'B',
    };
    const d1l_advert_data_t fuzzed = parse_ok(
        fuzz_regression, sizeof(fuzz_regression));
    assert(strcmp(fuzzed.name, "A________") == 0);

    const uint8_t empty_name[] = {0x83U};
    const d1l_advert_data_t empty = parse_ok(empty_name, sizeof(empty_name));
    assert(empty.type_code == 'R');
    assert(empty.name[0] == '\0');
}

int main(void)
{
    test_exact_cambridge_vector();
    test_signed_and_boundary_coordinates();
    test_every_optional_field_truncation_boundary();
    test_every_optional_field_combination();
    test_null_oversize_and_maximum_length();
    test_name_sanitization_and_type_mapping();
    return 0;
}
