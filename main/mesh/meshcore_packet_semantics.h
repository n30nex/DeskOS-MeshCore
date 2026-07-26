#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "meshcore_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    D1L_MESHCORE_PACKET_SEMANTIC_INVALID = 0,
    D1L_MESHCORE_PACKET_SEMANTIC_ADMIN_RESPONSE,
    D1L_MESHCORE_PACKET_SEMANTIC_CHANNEL_TEXT,
    D1L_MESHCORE_PACKET_SEMANTIC_DIRECT_MESSAGE,
    D1L_MESHCORE_PACKET_SEMANTIC_ACK,
    D1L_MESHCORE_PACKET_SEMANTIC_MULTIPART_ACK,
    D1L_MESHCORE_PACKET_SEMANTIC_PATH,
    D1L_MESHCORE_PACKET_SEMANTIC_TRACE,
    D1L_MESHCORE_PACKET_SEMANTIC_ADVERT,
    D1L_MESHCORE_PACKET_SEMANTIC_CONTROL,
} d1l_meshcore_packet_semantic_kind_t;

typedef struct {
    d1l_meshcore_packet_semantic_kind_t kind;
    d1l_meshcore_wire_packet_t wire;
    const uint8_t *body;
    uint16_t body_len;
} d1l_meshcore_packet_semantic_view_t;

/*
 * Classifies only the structural packet families admitted by the production
 * receive dispatcher. It intentionally does not claim decrypt/MAC/signature
 * validity. Rejection is side-effect free and leaves out_view fully zeroed.
 */
bool d1l_meshcore_packet_semantic_parse(
    const uint8_t *raw,
    size_t raw_len,
    d1l_meshcore_packet_semantic_view_t *out_view);

#ifdef __cplusplus
}
#endif
