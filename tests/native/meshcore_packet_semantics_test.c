#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mesh/meshcore_discovery.h"
#include "mesh/meshcore_packet_semantics.h"
#include "mesh/meshcore_trace.h"

#define DETERMINISTIC_CASES 100000U
#define TEST_BUFFER_BYTES 320U

typedef struct {
    uint64_t before;
    d1l_meshcore_packet_semantic_view_t view;
    uint64_t after;
} guarded_view_t;

static uint32_t s_prng = 0xd1a5e11U;

static uint32_t next_random(void)
{
    uint32_t x = s_prng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_prng = x;
    return x;
}

static size_t make_direct_frame(uint8_t *raw, uint8_t type,
                                const uint8_t *payload, size_t payload_len)
{
    assert(raw != NULL);
    assert(payload != NULL || payload_len == 0U);
    assert(payload_len <= D1L_MESHCORE_MAX_PACKET_PAYLOAD);
    raw[0] = (uint8_t)((type << 2U) | D1L_MESHCORE_ROUTE_DIRECT);
    raw[1] = 0U;
    if (payload_len > 0U) {
        memmove(&raw[2], payload, payload_len);
    }
    return 2U + payload_len;
}

static void assert_zeroed(const d1l_meshcore_packet_semantic_view_t *view)
{
    const d1l_meshcore_packet_semantic_view_t zero = {0};
    assert(memcmp(view, &zero, sizeof(zero)) == 0);
}

static void assert_view_equal(
    const d1l_meshcore_packet_semantic_view_t *left,
    const d1l_meshcore_packet_semantic_view_t *right)
{
    assert(left->kind == right->kind);
    assert(left->body == right->body);
    assert(left->body_len == right->body_len);
    assert(left->wire.header == right->wire.header);
    assert(left->wire.route == right->wire.route);
    assert(left->wire.type == right->wire.type);
    assert(left->wire.version == right->wire.version);
    assert(left->wire.transport_codes[0] == right->wire.transport_codes[0]);
    assert(left->wire.transport_codes[1] == right->wire.transport_codes[1]);
    assert(left->wire.path_len == right->wire.path_len);
    assert(left->wire.path_hash_bytes == right->wire.path_hash_bytes);
    assert(left->wire.path_hops == right->wire.path_hops);
    assert(left->wire.path == right->wire.path);
    assert(left->wire.path_byte_len == right->wire.path_byte_len);
    assert(left->wire.payload == right->wire.payload);
    assert(left->wire.payload_len == right->wire.payload_len);
}

static bool parse_guarded(const uint8_t *raw, size_t raw_len,
                          d1l_meshcore_packet_semantic_view_t *out)
{
    guarded_view_t guarded;
    memset(&guarded, 0xa5, sizeof(guarded));
    guarded.before = UINT64_C(0x0123456789abcdef);
    guarded.after = UINT64_C(0xfedcba9876543210);
    const bool accepted = d1l_meshcore_packet_semantic_parse(
        raw, raw_len, &guarded.view);
    assert(guarded.before == UINT64_C(0x0123456789abcdef));
    assert(guarded.after == UINT64_C(0xfedcba9876543210));
    if (!accepted) {
        assert_zeroed(&guarded.view);
    }
    if (out != NULL) {
        *out = guarded.view;
    }
    return accepted;
}

static void expect_type_length(uint8_t type, size_t payload_len,
                               bool expected,
                               d1l_meshcore_packet_semantic_kind_t kind)
{
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
    uint8_t payload[D1L_MESHCORE_MAX_PACKET_PAYLOAD] = {0};
    if (type == D1L_MESHCORE_PAYLOAD_MULTIPART && payload_len > 0U) {
        payload[0] = 0xf3U;
    } else if (type == D1L_MESHCORE_PAYLOAD_CONTROL && payload_len > 0U) {
        payload[0] = D1L_MESHCORE_DISCOVERY_RESPONSE_TYPE;
    }
    const size_t raw_len = make_direct_frame(raw, type, payload, payload_len);
    d1l_meshcore_packet_semantic_view_t view;
    const bool accepted = parse_guarded(raw, raw_len, &view);
    assert(accepted == expected);
    if (accepted) {
        assert(view.kind == kind);
        assert(view.wire.payload_len == payload_len);
    }
}

static void test_boundaries(void)
{
    d1l_meshcore_packet_semantic_view_t view;
    memset(&view, 0xa5, sizeof(view));
    assert(!d1l_meshcore_packet_semantic_parse(NULL, 0U, &view));
    assert_zeroed(&view);
    assert(!d1l_meshcore_packet_semantic_parse(NULL, 0U, NULL));

    const uint8_t tiny[] = {0U, 0U};
    for (size_t size = 0U; size <= sizeof(tiny); ++size) {
        assert(!parse_guarded(tiny, size, NULL));
    }

    expect_type_length(0x01U, 4U, false,
                       D1L_MESHCORE_PACKET_SEMANTIC_INVALID);
    expect_type_length(0x01U, 5U, true,
                       D1L_MESHCORE_PACKET_SEMANTIC_ADMIN_RESPONSE);
    expect_type_length(D1L_MESHCORE_PAYLOAD_GROUP_TEXT, 2U, false,
                       D1L_MESHCORE_PACKET_SEMANTIC_INVALID);
    expect_type_length(D1L_MESHCORE_PAYLOAD_GROUP_TEXT, 3U, true,
                       D1L_MESHCORE_PACKET_SEMANTIC_CHANNEL_TEXT);
    expect_type_length(D1L_MESHCORE_PAYLOAD_TEXT, 4U, false,
                       D1L_MESHCORE_PACKET_SEMANTIC_INVALID);
    expect_type_length(D1L_MESHCORE_PAYLOAD_TEXT, 5U, true,
                       D1L_MESHCORE_PACKET_SEMANTIC_DIRECT_MESSAGE);
    expect_type_length(D1L_MESHCORE_PAYLOAD_ACK, 3U, false,
                       D1L_MESHCORE_PACKET_SEMANTIC_INVALID);
    expect_type_length(D1L_MESHCORE_PAYLOAD_ACK, 4U, true,
                       D1L_MESHCORE_PACKET_SEMANTIC_ACK);
    expect_type_length(D1L_MESHCORE_PAYLOAD_ACK, 6U, true,
                       D1L_MESHCORE_PACKET_SEMANTIC_ACK);
    expect_type_length(D1L_MESHCORE_PAYLOAD_MULTIPART, 4U, false,
                       D1L_MESHCORE_PACKET_SEMANTIC_INVALID);
    expect_type_length(D1L_MESHCORE_PAYLOAD_MULTIPART, 5U, true,
                       D1L_MESHCORE_PACKET_SEMANTIC_MULTIPART_ACK);
    expect_type_length(D1L_MESHCORE_PAYLOAD_PATH, 4U, false,
                       D1L_MESHCORE_PACKET_SEMANTIC_INVALID);
    expect_type_length(D1L_MESHCORE_PAYLOAD_PATH, 5U, true,
                       D1L_MESHCORE_PACKET_SEMANTIC_PATH);
    expect_type_length(D1L_MESHCORE_PAYLOAD_TRACE, 8U, false,
                       D1L_MESHCORE_PACKET_SEMANTIC_INVALID);
    expect_type_length(D1L_MESHCORE_PAYLOAD_TRACE, 9U, true,
                       D1L_MESHCORE_PACKET_SEMANTIC_TRACE);
    expect_type_length(D1L_MESHCORE_PAYLOAD_TRACE, 10U, true,
                       D1L_MESHCORE_PACKET_SEMANTIC_TRACE);
    expect_type_length(D1L_MESHCORE_PAYLOAD_ADVERT, 99U, false,
                       D1L_MESHCORE_PACKET_SEMANTIC_INVALID);
    expect_type_length(D1L_MESHCORE_PAYLOAD_ADVERT, 100U, true,
                       D1L_MESHCORE_PACKET_SEMANTIC_ADVERT);
    expect_type_length(D1L_MESHCORE_PAYLOAD_ADVERT, 132U, true,
                       D1L_MESHCORE_PACKET_SEMANTIC_ADVERT);
    expect_type_length(D1L_MESHCORE_PAYLOAD_ADVERT, 133U, false,
                       D1L_MESHCORE_PACKET_SEMANTIC_INVALID);
    expect_type_length(D1L_MESHCORE_PAYLOAD_CONTROL, 5U, false,
                       D1L_MESHCORE_PACKET_SEMANTIC_INVALID);
    expect_type_length(D1L_MESHCORE_PAYLOAD_CONTROL, 6U, true,
                       D1L_MESHCORE_PACKET_SEMANTIC_CONTROL);

    uint8_t raw[256] = {0};
    uint8_t payload[5] = {0};
    size_t raw_len = make_direct_frame(raw, D1L_MESHCORE_PAYLOAD_MULTIPART,
                                       payload, sizeof(payload));
    assert(!parse_guarded(raw, raw_len, NULL));
    raw[2] = 0x13U;
    assert(parse_guarded(raw, raw_len, &view));
    assert(view.body == &raw[3] && view.body_len == 4U);

    raw_len = make_direct_frame(raw, D1L_MESHCORE_PAYLOAD_ACK,
                                payload, sizeof(payload));
    raw[0] |= 0x40U;
    assert(!parse_guarded(raw, raw_len, NULL));

    uint8_t trace_payload[D1L_MESHCORE_TRACE_FIXED_BYTES + 1U] = {0};
    raw_len = make_direct_frame(raw, D1L_MESHCORE_PAYLOAD_TRACE,
                                trace_payload, sizeof(trace_payload));
    raw[0] = (uint8_t)((D1L_MESHCORE_PAYLOAD_TRACE << 2U) |
                       D1L_MESHCORE_ROUTE_FLOOD);
    assert(!parse_guarded(raw, raw_len, NULL));

    raw[0] = (uint8_t)((D1L_MESHCORE_PAYLOAD_ACK << 2U) |
                       D1L_MESHCORE_ROUTE_TRANSPORT_DIRECT);
    raw[1] = raw[2] = raw[3] = raw[4] = 0U;
    raw[5] = 1U;
    raw[6] = 0xaaU;
    assert(!parse_guarded(raw, 7U, NULL));

    memset(raw, 0, sizeof(raw));
    raw[0] = (uint8_t)((D1L_MESHCORE_PAYLOAD_ACK << 2U) |
                       D1L_MESHCORE_ROUTE_DIRECT);
    raw[1] = 0xffU;
    raw[2] = 0U;
    assert(!parse_guarded(raw, 3U, NULL));
    assert(!parse_guarded(raw, 256U, NULL));
}

static void test_deterministic_cases(void)
{
    for (size_t case_index = 0U; case_index < DETERMINISTIC_CASES;
         ++case_index) {
        uint8_t raw[TEST_BUFFER_BYTES];
        for (size_t i = 0U; i < sizeof(raw); ++i) {
            raw[i] = (uint8_t)next_random();
        }
        size_t raw_len = next_random() % (sizeof(raw) + 1U);

        if ((case_index & 3U) == 0U) {
            static const uint8_t types[] = {
                0x01U,
                D1L_MESHCORE_PAYLOAD_TEXT,
                D1L_MESHCORE_PAYLOAD_ACK,
                D1L_MESHCORE_PAYLOAD_ADVERT,
                D1L_MESHCORE_PAYLOAD_GROUP_TEXT,
                D1L_MESHCORE_PAYLOAD_PATH,
                D1L_MESHCORE_PAYLOAD_TRACE,
                D1L_MESHCORE_PAYLOAD_MULTIPART,
                D1L_MESHCORE_PAYLOAD_CONTROL,
            };
            const uint8_t type = types[case_index %
                (sizeof(types) / sizeof(types[0]))];
            size_t payload_len = 1U +
                (next_random() % D1L_MESHCORE_MAX_PACKET_PAYLOAD);
            raw_len = make_direct_frame(raw, type, &raw[2], payload_len);
            if (type == D1L_MESHCORE_PAYLOAD_MULTIPART) {
                raw[2] = (uint8_t)((raw[2] & 0xf0U) |
                                   D1L_MESHCORE_PAYLOAD_ACK);
            } else if (type == D1L_MESHCORE_PAYLOAD_CONTROL) {
                raw[2] |= 0x80U;
            }
        }

        uint8_t original[TEST_BUFFER_BYTES];
        memcpy(original, raw, sizeof(raw));
        d1l_meshcore_packet_semantic_view_t first;
        d1l_meshcore_packet_semantic_view_t second;
        const bool accepted = parse_guarded(raw, raw_len, &first);
        const bool repeated = parse_guarded(raw, raw_len, &second);
        assert(accepted == repeated);
        assert(memcmp(raw, original, sizeof(raw)) == 0);
        if (!accepted) {
            assert_zeroed(&first);
            assert_zeroed(&second);
            continue;
        }

        assert_view_equal(&first, &second);
        assert(first.kind > D1L_MESHCORE_PACKET_SEMANTIC_INVALID);
        assert(first.kind <= D1L_MESHCORE_PACKET_SEMANTIC_CONTROL);
        assert(first.wire.version == D1L_MESHCORE_PAYLOAD_VER_1);
        assert(first.wire.payload >= raw);
        assert(first.wire.payload + first.wire.payload_len <= raw + raw_len);
        assert(first.body >= first.wire.payload);
        assert(first.body + first.body_len <=
               first.wire.payload + first.wire.payload_len);
        assert(first.body_len > 0U);
    }
}

int main(void)
{
    test_boundaries();
    test_deterministic_cases();
    puts("native MeshCore semantic packet parser: ok (100000 deterministic cases)");
    return 0;
}
