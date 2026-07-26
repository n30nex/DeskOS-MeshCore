#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define D1L_MESHCORE_DISCOVERY_REQUEST_TYPE 0x80U
#define D1L_MESHCORE_DISCOVERY_RESPONSE_TYPE 0x90U
#define D1L_MESHCORE_DISCOVERY_NODE_TYPE_CHAT 0x01U
#define D1L_MESHCORE_DISCOVERY_NODE_TYPE_REPEATER 0x02U
#define D1L_MESHCORE_DISCOVERY_NODE_TYPE_ROOM 0x03U
#define D1L_MESHCORE_DISCOVERY_NODE_TYPE_SENSOR 0x04U
#define D1L_MESHCORE_DISCOVERY_FILTER_ALL 0x1eU
#define D1L_MESHCORE_DISCOVERY_REQUEST_BYTES 10U
#define D1L_MESHCORE_DISCOVERY_RESPONSE_BYTES 38U
#define D1L_MESHCORE_DISCOVERY_PUBLIC_KEY_BYTES 32U
#define D1L_MESHCORE_DISCOVERY_SESSION_MS 60000U
#define D1L_MESHCORE_DISCOVERY_MAX_RESULTS 24U

typedef struct {
    uint8_t node_type;
    int8_t remote_snr_quarter_db;
    uint32_t tag;
    uint8_t public_key[D1L_MESHCORE_DISCOVERY_PUBLIC_KEY_BYTES];
} d1l_meshcore_discovery_wire_result_t;

static inline uint32_t d1l_meshcore_discovery_read_le32(
    const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

static inline void d1l_meshcore_discovery_write_le32(
    uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xffU);
    destination[1] = (uint8_t)((value >> 8U) & 0xffU);
    destination[2] = (uint8_t)((value >> 16U) & 0xffU);
    destination[3] = (uint8_t)((value >> 24U) & 0xffU);
}

static inline bool d1l_meshcore_discovery_node_type_valid(uint8_t node_type)
{
    return node_type >= D1L_MESHCORE_DISCOVERY_NODE_TYPE_CHAT &&
           node_type <= D1L_MESHCORE_DISCOVERY_NODE_TYPE_SENSOR;
}

static inline bool d1l_meshcore_discovery_public_key_nonzero(
    const uint8_t public_key[D1L_MESHCORE_DISCOVERY_PUBLIC_KEY_BYTES])
{
    uint8_t combined = 0U;
    for (size_t i = 0U; i < D1L_MESHCORE_DISCOVERY_PUBLIC_KEY_BYTES; ++i) {
        combined |= public_key[i];
    }
    return combined != 0U;
}

static inline bool d1l_meshcore_discovery_build_request(
    uint32_t tag,
    uint8_t destination[D1L_MESHCORE_DISCOVERY_REQUEST_BYTES])
{
    if (!destination || tag == 0U) {
        return false;
    }
    memset(destination, 0, D1L_MESHCORE_DISCOVERY_REQUEST_BYTES);
    destination[0] = D1L_MESHCORE_DISCOVERY_REQUEST_TYPE;
    destination[1] = D1L_MESHCORE_DISCOVERY_FILTER_ALL;
    d1l_meshcore_discovery_write_le32(&destination[2], tag);
    /* A zero "since" timestamp asks every compatible local node to answer. */
    d1l_meshcore_discovery_write_le32(&destination[6], 0U);
    return true;
}

static inline bool d1l_meshcore_discovery_parse_response(
    const uint8_t *payload,
    size_t payload_len,
    uint32_t expected_tag,
    d1l_meshcore_discovery_wire_result_t *out_result)
{
    if (!payload || !out_result ||
        payload_len != D1L_MESHCORE_DISCOVERY_RESPONSE_BYTES ||
        (payload[0] & 0xf0U) != D1L_MESHCORE_DISCOVERY_RESPONSE_TYPE) {
        return false;
    }
    const uint8_t node_type = payload[0] & 0x0fU;
    const uint32_t tag = d1l_meshcore_discovery_read_le32(&payload[2]);
    if (!d1l_meshcore_discovery_node_type_valid(node_type) ||
        tag == 0U || tag != expected_tag ||
        !d1l_meshcore_discovery_public_key_nonzero(&payload[6])) {
        return false;
    }

    d1l_meshcore_discovery_wire_result_t result = {
        .node_type = node_type,
        .remote_snr_quarter_db = (int8_t)payload[1],
        .tag = tag,
    };
    memcpy(result.public_key, &payload[6], sizeof(result.public_key));
    *out_result = result;
    return true;
}
