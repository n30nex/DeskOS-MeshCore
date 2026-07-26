#pragma once

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "Packet.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

extern "C" {
#include "mesh/meshcore_wire.h"
}

static_assert(MAX_PACKET_PAYLOAD == 184,
              "Pinned MeshCore packet payload limit changed");
static_assert(MAX_PATH_SIZE == 64, "Pinned MeshCore path limit changed");
static_assert(MAX_TRANS_UNIT == 255, "Pinned MeshCore MTU changed");
static_assert(ROUTE_TYPE_TRANSPORT_FLOOD == 0x00,
              "Pinned MeshCore transport-flood route changed");
static_assert(ROUTE_TYPE_FLOOD == 0x01,
              "Pinned MeshCore flood route changed");
static_assert(ROUTE_TYPE_DIRECT == 0x02,
              "Pinned MeshCore direct route changed");
static_assert(ROUTE_TYPE_TRANSPORT_DIRECT == 0x03,
              "Pinned MeshCore transport-direct route changed");
static_assert(PAYLOAD_TYPE_TXT_MSG == 0x02,
              "Pinned MeshCore text payload type changed");
static_assert(PAYLOAD_TYPE_ACK == 0x03,
              "Pinned MeshCore ACK payload type changed");
static_assert(PAYLOAD_TYPE_ADVERT == 0x04,
              "Pinned MeshCore advert payload type changed");
static_assert(PAYLOAD_TYPE_GRP_TXT == 0x05,
              "Pinned MeshCore group-text payload type changed");
static_assert(PAYLOAD_TYPE_PATH == 0x08,
              "Pinned MeshCore path payload type changed");
static_assert(PAYLOAD_TYPE_TRACE == 0x09,
              "Pinned MeshCore TRACE payload type changed");
static_assert(PAYLOAD_TYPE_MULTIPART == 0x0A,
              "Pinned MeshCore multipart payload type changed");
static_assert(PAYLOAD_TYPE_CONTROL == 0x0B,
              "Pinned MeshCore control payload type changed");
static_assert(D1L_MESHCORE_PAYLOAD_VER_1 == PAYLOAD_VER_1,
              "Production and pinned v1 payload versions differ");
static_assert(D1L_MESHCORE_MAX_PACKET_PAYLOAD == MAX_PACKET_PAYLOAD,
              "Production and pinned payload limits differ");
static_assert(D1L_MESHCORE_MAX_PATH_BYTES == MAX_PATH_SIZE,
              "Production and pinned path limits differ");
static_assert(D1L_MESHCORE_MAX_RAW_PACKET == MAX_TRANS_UNIT,
              "Production and pinned MTUs differ");
static_assert(D1L_MESHCORE_ROUTE_TRANSPORT_FLOOD == ROUTE_TYPE_TRANSPORT_FLOOD,
              "Production transport-flood route differs");
static_assert(D1L_MESHCORE_ROUTE_FLOOD == ROUTE_TYPE_FLOOD,
              "Production flood route differs");
static_assert(D1L_MESHCORE_ROUTE_DIRECT == ROUTE_TYPE_DIRECT,
              "Production direct route differs");
static_assert(D1L_MESHCORE_ROUTE_TRANSPORT_DIRECT == ROUTE_TYPE_TRANSPORT_DIRECT,
              "Production transport-direct route differs");
static_assert(D1L_MESHCORE_PAYLOAD_TEXT == PAYLOAD_TYPE_TXT_MSG,
              "Production text payload type differs");
static_assert(D1L_MESHCORE_PAYLOAD_ACK == PAYLOAD_TYPE_ACK,
              "Production ACK payload type differs");
static_assert(D1L_MESHCORE_PAYLOAD_ADVERT == PAYLOAD_TYPE_ADVERT,
              "Production advert payload type differs");
static_assert(D1L_MESHCORE_PAYLOAD_GROUP_TEXT == PAYLOAD_TYPE_GRP_TXT,
              "Production group-text payload type differs");
static_assert(D1L_MESHCORE_PAYLOAD_PATH == PAYLOAD_TYPE_PATH,
              "Production PATH payload type differs");
static_assert(D1L_MESHCORE_PAYLOAD_TRACE == PAYLOAD_TYPE_TRACE,
              "Production TRACE payload type differs");
static_assert(D1L_MESHCORE_PAYLOAD_MULTIPART == PAYLOAD_TYPE_MULTIPART,
              "Production multipart payload type differs");
static_assert(D1L_MESHCORE_PAYLOAD_CONTROL == PAYLOAD_TYPE_CONTROL,
              "Production control payload type differs");

#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__)
#error "The checked MeshCore Packet shim requires compiler byte-order macros"
#elif __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Pinned Packet.cpp writes native uint16_t transport codes; use little endian"
#endif
