#include "advert_data.h"

#include <string.h>

#define D1L_ADVERT_TYPE_CHAT 0x01U
#define D1L_ADVERT_TYPE_REPEATER 0x02U
#define D1L_ADVERT_TYPE_ROOM 0x03U
#define D1L_ADVERT_TYPE_SENSOR 0x04U
#define D1L_ADVERT_LOCATION_MASK 0x10U
#define D1L_ADVERT_FEATURE1_MASK 0x20U
#define D1L_ADVERT_FEATURE2_MASK 0x40U
#define D1L_ADVERT_NAME_MASK 0x80U

static uint32_t read_le_u32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static int32_t read_le_i32(const uint8_t *src)
{
    const uint32_t raw = read_le_u32(src);
    return raw <= 0x7fffffffU ? (int32_t)raw :
                               (int32_t)((int64_t)raw - 0x100000000LL);
}

static char advert_type_code(uint8_t flags)
{
    switch (flags & 0x0fU) {
    case D1L_ADVERT_TYPE_CHAT:
        return 'C';
    case D1L_ADVERT_TYPE_REPEATER:
        return 'P';
    case D1L_ADVERT_TYPE_ROOM:
        return 'R';
    case D1L_ADVERT_TYPE_SENSOR:
        return 'S';
    default:
        return 'N';
    }
}

static bool advert_location_in_bounds(int32_t lat_e6, int32_t lon_e6)
{
    return lat_e6 >= -90000000 && lat_e6 <= 90000000 &&
           lon_e6 >= -180000000 && lon_e6 <= 180000000;
}

bool d1l_advert_data_parse(const uint8_t *app_data, size_t app_data_len,
                           d1l_advert_data_t *out)
{
    if (!out) {
        return false;
    }

    d1l_advert_data_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.type_code = 'N';
    *out = parsed;
    if (app_data_len > D1L_ADVERT_DATA_MAX_LEN || (!app_data && app_data_len > 0U)) {
        return false;
    }
    if (app_data_len == 0U) {
        return true;
    }

    const uint8_t flags = app_data[0];
    parsed.type_code = advert_type_code(flags);
    size_t i = 1U;
    if ((flags & D1L_ADVERT_LOCATION_MASK) != 0U) {
        if (app_data_len - i < 8U) {
            return false;
        }
        parsed.lat_e6 = read_le_i32(&app_data[i]);
        parsed.lon_e6 = read_le_i32(&app_data[i + 4U]);
        if (!advert_location_in_bounds(parsed.lat_e6, parsed.lon_e6)) {
            return false;
        }
        parsed.location_valid = true;
        i += 8U;
    }
    if ((flags & D1L_ADVERT_FEATURE1_MASK) != 0U) {
        if (app_data_len - i < 2U) {
            return false;
        }
        i += 2U;
    }
    if ((flags & D1L_ADVERT_FEATURE2_MASK) != 0U) {
        if (app_data_len - i < 2U) {
            return false;
        }
        i += 2U;
    }
    if ((flags & D1L_ADVERT_NAME_MASK) == 0U || i == app_data_len) {
        *out = parsed;
        return true;
    }

    size_t name_len = 0U;
    while (i < app_data_len && name_len + 1U < sizeof(parsed.name)) {
        unsigned char c = app_data[i++];
        if (c == '\0') {
            break;
        }
        if (c < 32U || c > 126U || c == '"' || c == '\\') {
            c = '_';
        }
        parsed.name[name_len++] = (char)c;
    }
    parsed.name[name_len] = '\0';
    *out = parsed;
    return true;
}
