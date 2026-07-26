#include "meshcore_packet_semantics.h"

#include <string.h>

#include "advert_data.h"
#include "meshcore_trace.h"

#define D1L_MESHCORE_ADMIN_RESPONSE_TYPE 0x01U
#define D1L_MESHCORE_CIPHER_MAC_SIZE 2U
#define D1L_MESHCORE_ADVERT_BASE_BYTES (32U + 4U + 64U)

static bool classify(const uint8_t *raw,
                     size_t raw_len,
                     const d1l_meshcore_wire_packet_t *packet,
                     d1l_meshcore_packet_semantic_view_t *view)
{
    view->wire = *packet;
    view->body = packet->payload;
    view->body_len = packet->payload_len;

    switch (packet->type) {
    case D1L_MESHCORE_ADMIN_RESPONSE_TYPE:
        if (packet->payload_len <= 2U + D1L_MESHCORE_CIPHER_MAC_SIZE) {
            return false;
        }
        view->kind = D1L_MESHCORE_PACKET_SEMANTIC_ADMIN_RESPONSE;
        return true;
    case D1L_MESHCORE_PAYLOAD_GROUP_TEXT:
        if (packet->payload_len < 3U) {
            return false;
        }
        view->kind = D1L_MESHCORE_PACKET_SEMANTIC_CHANNEL_TEXT;
        return true;
    case D1L_MESHCORE_PAYLOAD_TEXT:
        if (packet->payload_len <= 2U + D1L_MESHCORE_CIPHER_MAC_SIZE) {
            return false;
        }
        view->kind = D1L_MESHCORE_PACKET_SEMANTIC_DIRECT_MESSAGE;
        return true;
    case D1L_MESHCORE_PAYLOAD_ACK:
        if (packet->payload_len < 4U) {
            return false;
        }
        view->kind = D1L_MESHCORE_PACKET_SEMANTIC_ACK;
        return true;
    case D1L_MESHCORE_PAYLOAD_MULTIPART:
        if (packet->payload_len < 5U ||
            (packet->payload[0] & 0x0fU) != D1L_MESHCORE_PAYLOAD_ACK) {
            return false;
        }
        view->kind = D1L_MESHCORE_PACKET_SEMANTIC_MULTIPART_ACK;
        view->body = &packet->payload[1];
        view->body_len = packet->payload_len - 1U;
        return true;
    case D1L_MESHCORE_PAYLOAD_PATH:
        if (packet->payload_len <= 2U + D1L_MESHCORE_CIPHER_MAC_SIZE) {
            return false;
        }
        view->kind = D1L_MESHCORE_PACKET_SEMANTIC_PATH;
        return true;
    case D1L_MESHCORE_PAYLOAD_TRACE:
        if (d1l_meshcore_trace_classify(raw, raw_len, NULL) ==
            D1L_MESHCORE_TRACE_FRAME_MALFORMED) {
            return false;
        }
        view->kind = D1L_MESHCORE_PACKET_SEMANTIC_TRACE;
        return true;
    case D1L_MESHCORE_PAYLOAD_ADVERT:
        if (packet->payload_len < D1L_MESHCORE_ADVERT_BASE_BYTES ||
            packet->payload_len >
                D1L_MESHCORE_ADVERT_BASE_BYTES + D1L_ADVERT_DATA_MAX_LEN) {
            return false;
        }
        view->kind = D1L_MESHCORE_PACKET_SEMANTIC_ADVERT;
        return true;
    case D1L_MESHCORE_PAYLOAD_CONTROL:
        if (packet->route != D1L_MESHCORE_ROUTE_DIRECT ||
            packet->path_hops != 0U || packet->payload_len < 6U ||
            (packet->payload[0] & 0x80U) == 0U) {
            return false;
        }
        view->kind = D1L_MESHCORE_PACKET_SEMANTIC_CONTROL;
        return true;
    default:
        return false;
    }
}

bool d1l_meshcore_packet_semantic_parse(
    const uint8_t *raw,
    size_t raw_len,
    d1l_meshcore_packet_semantic_view_t *out_view)
{
    if (!out_view) {
        return false;
    }
    memset(out_view, 0, sizeof(*out_view));

    d1l_meshcore_wire_packet_t packet = {0};
    d1l_meshcore_packet_semantic_view_t view = {0};
    if (!d1l_meshcore_wire_decode_v1(raw, raw_len, &packet) ||
        !classify(raw, raw_len, &packet, &view)) {
        return false;
    }

    *out_view = view;
    return true;
}
