#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mesh/meshcore_discovery.h"

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,          \
                    __LINE__, #condition);                                      \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static int test_request(void)
{
    const uint32_t tag = UINT32_C(0x78563412);
    uint8_t request[D1L_MESHCORE_DISCOVERY_REQUEST_BYTES] = {0xa5U};
    CHECK(!d1l_meshcore_discovery_build_request(0U, request));
    CHECK(!d1l_meshcore_discovery_build_request(tag, NULL));
    CHECK(d1l_meshcore_discovery_build_request(tag, request));
    const uint8_t expected[D1L_MESHCORE_DISCOVERY_REQUEST_BYTES] = {
        D1L_MESHCORE_DISCOVERY_REQUEST_TYPE,
        D1L_MESHCORE_DISCOVERY_FILTER_ALL,
        0x12U, 0x34U, 0x56U, 0x78U,
        0x00U, 0x00U, 0x00U, 0x00U,
    };
    CHECK(memcmp(request, expected, sizeof(expected)) == 0);
    CHECK(d1l_meshcore_discovery_request_valid(request, sizeof(request)));
    request[0] = 0x81U;
    CHECK(d1l_meshcore_discovery_request_valid(request, 6U));
    CHECK(!d1l_meshcore_discovery_request_valid(request, 7U));
    request[0] = 0x90U;
    CHECK(!d1l_meshcore_discovery_request_valid(request, sizeof(request)));
    request[0] = 0x80U;
    request[1] = 0xffU;
    CHECK(!d1l_meshcore_discovery_request_valid(request, sizeof(request)));
    request[1] = 4U;
    memset(&request[2], 0, 4U);
    CHECK(!d1l_meshcore_discovery_request_valid(request, sizeof(request)));
    return 0;
}

static int test_response(void)
{
    const uint32_t tag = UINT32_C(0x12345678);
    uint8_t response[D1L_MESHCORE_DISCOVERY_RESPONSE_BYTES] = {0};
    response[0] = D1L_MESHCORE_DISCOVERY_RESPONSE_TYPE |
                  D1L_MESHCORE_DISCOVERY_NODE_TYPE_REPEATER;
    response[1] = (uint8_t)(int8_t)-9;
    d1l_meshcore_discovery_write_le32(&response[2], tag);
    for (size_t i = 0U; i < D1L_MESHCORE_DISCOVERY_PUBLIC_KEY_BYTES; ++i) {
        response[6U + i] = (uint8_t)(i + 1U);
    }

    d1l_meshcore_discovery_wire_result_t parsed = {0};
    CHECK(d1l_meshcore_discovery_parse_response(
        response, sizeof(response), tag, &parsed));
    CHECK(parsed.node_type == D1L_MESHCORE_DISCOVERY_NODE_TYPE_REPEATER);
    CHECK(parsed.remote_snr_quarter_db == -9);
    CHECK(parsed.tag == tag);
    CHECK(parsed.public_key_bytes == 32U);
    CHECK(memcmp(parsed.public_key, &response[6], sizeof(parsed.public_key)) == 0);

    CHECK(!d1l_meshcore_discovery_parse_response(
        response, sizeof(response) - 1U, tag, &parsed));
    CHECK(d1l_meshcore_discovery_parse_response(
        response, 14U, tag, &parsed));
    CHECK(parsed.public_key_bytes == 8U);
    CHECK(memcmp(parsed.public_key, &response[6], 8U) == 0);
    for (size_t i = 8U; i < sizeof(parsed.public_key); ++i) {
        CHECK(parsed.public_key[i] == 0U);
    }
    CHECK(!d1l_meshcore_discovery_parse_response(
        response, sizeof(response), tag + 1U, &parsed));
    response[0] = D1L_MESHCORE_DISCOVERY_RESPONSE_TYPE;
    CHECK(!d1l_meshcore_discovery_parse_response(
        response, sizeof(response), tag, &parsed));
    response[0] = D1L_MESHCORE_DISCOVERY_REQUEST_TYPE |
                  D1L_MESHCORE_DISCOVERY_NODE_TYPE_REPEATER;
    CHECK(!d1l_meshcore_discovery_parse_response(
        response, sizeof(response), tag, &parsed));
    response[0] = D1L_MESHCORE_DISCOVERY_RESPONSE_TYPE |
                  D1L_MESHCORE_DISCOVERY_NODE_TYPE_CHAT;
    memset(&response[6], 0, D1L_MESHCORE_DISCOVERY_PUBLIC_KEY_BYTES);
    CHECK(!d1l_meshcore_discovery_parse_response(
        response, sizeof(response), tag, &parsed));
    return 0;
}

int main(void)
{
    CHECK(test_request() == 0);
    CHECK(test_response() == 0);
    puts("meshcore_discovery_test: ok");
    return 0;
}
