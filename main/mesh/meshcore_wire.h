#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D1L_MESHCORE_ROUTE_TRANSPORT_FLOOD 0x00U
#define D1L_MESHCORE_ROUTE_FLOOD 0x01U
#define D1L_MESHCORE_ROUTE_DIRECT 0x02U
#define D1L_MESHCORE_ROUTE_TRANSPORT_DIRECT 0x03U

#define D1L_MESHCORE_PAYLOAD_TEXT 0x02U
#define D1L_MESHCORE_PAYLOAD_ACK 0x03U
#define D1L_MESHCORE_PAYLOAD_ADVERT 0x04U
#define D1L_MESHCORE_PAYLOAD_GROUP_TEXT 0x05U
#define D1L_MESHCORE_PAYLOAD_PATH 0x08U
#define D1L_MESHCORE_PAYLOAD_TRACE 0x09U
#define D1L_MESHCORE_PAYLOAD_MULTIPART 0x0AU
#define D1L_MESHCORE_PAYLOAD_CONTROL 0x0BU

#define D1L_MESHCORE_PAYLOAD_VER_1 0x00U

#define D1L_MESHCORE_HEADER_GROUP_TEXT_FLOOD \
    ((uint8_t)((D1L_MESHCORE_PAYLOAD_GROUP_TEXT << 2) | D1L_MESHCORE_ROUTE_FLOOD))
#define D1L_MESHCORE_HEADER_DM_TEXT_FLOOD \
    ((uint8_t)((D1L_MESHCORE_PAYLOAD_TEXT << 2) | D1L_MESHCORE_ROUTE_FLOOD))
#define D1L_MESHCORE_HEADER_DM_TEXT_DIRECT \
    ((uint8_t)((D1L_MESHCORE_PAYLOAD_TEXT << 2) | D1L_MESHCORE_ROUTE_DIRECT))

#define D1L_MESHCORE_MAX_RAW_PACKET 255U
#define D1L_MESHCORE_MAX_PATH_BYTES 64U
#define D1L_MESHCORE_MAX_PACKET_PAYLOAD 184U

typedef struct {
    uint8_t header;
    uint8_t route;
    uint8_t type;
    uint8_t version;
    uint16_t transport_codes[2];
    uint8_t path_len;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    const uint8_t *path;
    uint8_t path_byte_len;
    const uint8_t *payload;
    uint16_t payload_len;
} d1l_meshcore_wire_packet_t;

bool d1l_meshcore_wire_path_len_valid(uint8_t path_len);
uint8_t d1l_meshcore_wire_path_hash_size(uint8_t path_len);
uint8_t d1l_meshcore_wire_path_hash_count(uint8_t path_len);
uint8_t d1l_meshcore_wire_path_byte_len(uint8_t path_len);

bool d1l_meshcore_wire_decode(const uint8_t *raw,
                              size_t size,
                              d1l_meshcore_wire_packet_t *out_packet);

bool d1l_meshcore_wire_decode_v1(const uint8_t *raw,
                                 size_t size,
                                 d1l_meshcore_wire_packet_t *out_packet);

bool d1l_meshcore_wire_write_prefix(uint8_t header,
                                    uint16_t transport_code_0,
                                    uint16_t transport_code_1,
                                    uint8_t path_len,
                                    const uint8_t *path,
                                    uint8_t *dest,
                                    size_t dest_size,
                                    size_t *out_len);

bool d1l_meshcore_wire_encode(const d1l_meshcore_wire_packet_t *packet,
                              uint8_t *dest,
                              size_t dest_size,
                              size_t *out_len);

#ifdef __cplusplus
}
#endif
