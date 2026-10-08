#pragma once

#include <stdint.h>

static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *data,
                                      uint32_t length)
{
    crc = ~crc;
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}
