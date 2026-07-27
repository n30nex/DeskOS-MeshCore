#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mesh/meshcore_admin_dispatch.h"

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,          \
                    __LINE__, #condition);                                      \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static const uint8_t PEER[32] = {
    0x53U, 0x45U, 0x52U, 0x56U, 0x24U, 0x25U, 0x26U, 0x27U,
    0x28U, 0x29U, 0x2AU, 0x2BU, 0x2CU, 0x2DU, 0x2EU, 0x2FU,
    0x30U, 0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U,
    0x38U, 0x39U, 0x3AU, 0x3BU, 0x3CU, 0x3DU, 0x3EU, 0x3FU,
};
static const uint8_t OTHER_PEER[32] = {
    0x53U, 0x45U, 0x52U, 0x56U, 0xD4U, 0xD5U, 0xD6U, 0xD7U,
    0xD8U, 0xD9U, 0xDAU, 0xDBU, 0xDCU, 0xDDU, 0xDEU, 0xDFU,
    0xC0U, 0xC1U, 0xC2U, 0xC3U, 0xC4U, 0xC5U, 0xC6U, 0xC7U,
    0xC8U, 0xC9U, 0xCAU, 0xCBU, 0xCCU, 0xCDU, 0xCEU, 0xCFU,
};
static const uint8_t LOCAL[32] = {
    0xCAU, 0xFEU, 0xBAU, 0xBEU, 0x04U, 0x05U, 0x06U, 0x07U,
    0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU,
    0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
    0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU,
};
static const uint8_t SECRET[32] = {
    0x80U, 0x81U, 0x82U, 0x83U, 0x84U, 0x85U, 0x86U, 0x87U,
    0x88U, 0x89U, 0x8AU, 0x8BU, 0x8CU, 0x8DU, 0x8EU, 0x8FU,
    0x90U, 0x91U, 0x92U, 0x93U, 0x94U, 0x95U, 0x96U, 0x97U,
    0x98U, 0x99U, 0x9AU, 0x9BU, 0x9CU, 0x9DU, 0x9EU, 0x9FU,
};

static void write_le16(uint8_t *dest, uint16_t value)
{
    dest[0] = (uint8_t)value;
    dest[1] = (uint8_t)(value >> 8U);
}

static void write_le32(uint8_t *dest, uint32_t value)
{
    dest[0] = (uint8_t)value;
    dest[1] = (uint8_t)(value >> 8U);
    dest[2] = (uint8_t)(value >> 16U);
    dest[3] = (uint8_t)(value >> 24U);
}

static int all_zero(const void *value, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)value;
    for (size_t i = 0U; i < size; ++i) {
        if (bytes[i] != 0U) {
            return 0;
        }
    }
    return 1;
}

static void login_response_for_role(uint8_t response[16], uint8_t uniqueness,
                                    uint8_t firmware_level)
{
    memset(response, 0, 16U);
    write_le32(response, 0x55660000U | uniqueness);
    response[6] = 1U;
    response[7] = D1L_MESHCORE_ADMIN_PERMISSION_ADMIN;
    response[8] = uniqueness;
    response[9] = 0x20U;
    response[10] = 0x30U;
    response[11] = 0x40U;
    response[12] = firmware_level;
}

static void login_response(uint8_t response[16], uint8_t uniqueness)
{
    login_response_for_role(response, uniqueness, 2U);
}

static size_t status_response(uint8_t response[64], uint32_t tag)
{
    memset(response, 0, 64U);
    size_t offset = 0U;
    write_le32(&response[offset], tag); offset += 4U;
    write_le16(&response[offset], 3700U); offset += 2U;
    write_le16(&response[offset], 7U); offset += 2U;
    write_le16(&response[offset], (uint16_t)-117); offset += 2U;
    write_le16(&response[offset], (uint16_t)-83); offset += 2U;
    for (uint32_t value = 1U; value <= 8U; ++value) {
        write_le32(&response[offset], value); offset += 4U;
    }
    write_le16(&response[offset], 9U); offset += 2U;
    write_le16(&response[offset], (uint16_t)-20); offset += 2U;
    write_le16(&response[offset], 10U); offset += 2U;
    write_le16(&response[offset], 11U); offset += 2U;
    write_le32(&response[offset], 12U); offset += 4U;
    write_le32(&response[offset], 13U); offset += 4U;
    return offset;
}

static int begin(d1l_meshcore_admin_session_t *session, uint64_t deadline,
                 uint64_t idle_timeout, uint64_t absolute_timeout)
{
    return d1l_meshcore_admin_begin_login(
        session, D1L_MESHCORE_ADMIN_ROLE_REPEATER, PEER, LOCAL, SECRET,
        deadline, idle_timeout, absolute_timeout);
}

static int test_codec_and_repeater_status(void)
{
    uint8_t request[D1L_MESHCORE_ADMIN_MAX_LOGIN_REQUEST_BYTES] = {0};
    size_t request_len = 0U;
    static const uint8_t password[] = {'a', 'd', 'm', 'i', 'n'};
    CHECK(d1l_meshcore_admin_encode_login_request(
        D1L_MESHCORE_ADMIN_ROLE_REPEATER, 0x01020304U, 0U, password,
        sizeof(password), request, sizeof(request), &request_len));
    static const uint8_t expected_login[] = {
        0x04U, 0x03U, 0x02U, 0x01U, 'a', 'd', 'm', 'i', 'n'};
    CHECK(request_len == sizeof(expected_login));
    CHECK(memcmp(request, expected_login, sizeof(expected_login)) == 0);

    uint8_t status_request[D1L_MESHCORE_ADMIN_REQUEST_BYTES] = {0};
    CHECK(d1l_meshcore_admin_encode_status_request(
        0x01020305U, 0xA1A2A3A4U, status_request));
    static const uint8_t expected_status_request[] = {
        0x05U, 0x03U, 0x02U, 0x01U, 0x01U, 0U, 0U, 0U, 0U,
        0xA4U, 0xA3U, 0xA2U, 0xA1U};
    CHECK(memcmp(status_request, expected_status_request,
                 sizeof(expected_status_request)) == 0);

    d1l_meshcore_admin_session_t session = {0};
    d1l_meshcore_admin_replay_cache_t replay = {0};
    CHECK(begin(&session, 500U, 100U, 500U));
    uint8_t login[16];
    login_response(login, 0x10U);
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, OTHER_PEER, login, sizeof(login), 400U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED);
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, PEER, login, sizeof(login), 400U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.idle_deadline_us == 500U);
    CHECK(session.absolute_deadline_us == 900U);
    CHECK(d1l_meshcore_admin_begin_status_request(
        &session, 0x01020305U, 450U, 700U));
    uint8_t response[64];
    CHECK(status_response(response, 0x01020305U) ==
          D1L_MESHCORE_ADMIN_REPEATER_STATUS_BYTES);
    CHECK(d1l_meshcore_admin_accept_status_response(
              &session, PEER, response, sizeof(response), 480U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.idle_deadline_us == 580U);
    CHECK(session.status.battery_millivolts == 3700U);
    CHECK(session.status.receive_errors == 13U);
    return 0;
}

static int test_exact_deadlines_and_zeroization(void)
{
    d1l_meshcore_admin_session_t session = {0};
    d1l_meshcore_admin_replay_cache_t replay = {0};
    uint8_t login[16];
    login_response(login, 0x11U);
    CHECK(begin(&session, 500U, 100U, 300U));
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, PEER, login, sizeof(login), 500U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_EXPIRED);
    CHECK(session.state == D1L_MESHCORE_ADMIN_TIMED_OUT);
    CHECK(all_zero(session.session_secret, sizeof(session.session_secret)));
    CHECK(all_zero(session.local_public_key, sizeof(session.local_public_key)));

    CHECK(begin(&session, 700U, 100U, 150U));
    login_response(login, 0x12U);
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, PEER, login, sizeof(login), 600U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.idle_deadline_us == 700U);
    CHECK(session.absolute_deadline_us == 750U);
    CHECK(d1l_meshcore_admin_begin_status_request(
        &session, 0x01020306U, 650U, 720U));
    uint8_t malformed[64] = {0};
    (void)status_response(malformed, 0x01020306U);
    malformed[63] = 1U;
    CHECK(d1l_meshcore_admin_accept_status_response(
              &session, PEER, malformed, sizeof(malformed), 690U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED);
    CHECK(session.idle_deadline_us == 700U);
    CHECK(d1l_meshcore_admin_expire_if_due(&session, 700U));
    CHECK(all_zero(session.session_secret, sizeof(session.session_secret)));

    CHECK(begin(&session, 900U, 100U, 150U));
    login_response(login, 0x13U);
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, PEER, login, sizeof(login), 800U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(d1l_meshcore_admin_begin_status_request(
        &session, 0x01020307U, 810U, 850U));
    CHECK(d1l_meshcore_admin_expire_if_due(&session, 850U));
    return 0;
}

static int test_login_replay_and_room_no_history(void)
{
    d1l_meshcore_admin_session_t session = {0};
    d1l_meshcore_admin_replay_cache_t replay = {0};
    uint8_t login[16];
    login_response(login, 0x21U);
    CHECK(begin(&session, 200U, 100U, 300U));
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, PEER, login, sizeof(login), 100U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    d1l_meshcore_admin_reset(&session);
    CHECK(begin(&session, 400U, 100U, 300U));
    login_response(login, 0x22U);
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, PEER, login, sizeof(login), 300U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    d1l_meshcore_admin_reset(&session);
    CHECK(begin(&session, 600U, 100U, 300U));
    login_response(login, 0x21U);
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, PEER, login, sizeof(login), 500U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_REPLAYED);
    CHECK(session.state ==
          D1L_MESHCORE_ADMIN_VOLATILE_REPLAY_REJECTED);
    CHECK(all_zero(session.session_secret, sizeof(session.session_secret)));

    uint8_t room_request[D1L_MESHCORE_ADMIN_MAX_LOGIN_REQUEST_BYTES] = {0};
    size_t room_request_len = 0U;
    static const uint8_t password[] = {'r', 'o', 'o', 'm'};
    CHECK(d1l_meshcore_admin_encode_login_request(
        D1L_MESHCORE_ADMIN_ROLE_ROOM, 0x01020304U,
        0x01020304U, password,
        sizeof(password), room_request, sizeof(room_request),
        &room_request_len));
    static const uint8_t expected_room_request[] = {
        0x04U, 0x03U, 0x02U, 0x01U,
        0x04U, 0x03U, 0x02U, 0x01U,
        'r', 'o', 'o', 'm'};
    CHECK(room_request_len == sizeof(expected_room_request));
    CHECK(memcmp(room_request, expected_room_request,
                 sizeof(expected_room_request)) == 0);

    CHECK(d1l_meshcore_admin_begin_login(
        &session, D1L_MESHCORE_ADMIN_ROLE_ROOM, PEER, LOCAL, SECRET,
        700U, 100U, 300U));
    login_response_for_role(login, 0x23U, 1U);
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, PEER, login, sizeof(login), 650U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.role == D1L_MESHCORE_ADMIN_ROLE_ROOM);
    CHECK(session.firmware_level == 1U);
    return 0;
}

static int authenticate_repeater(d1l_meshcore_admin_session_t *session,
                                 d1l_meshcore_admin_replay_cache_t *replay,
                                 uint8_t uniqueness, uint64_t now_us)
{
    uint8_t login[16];
    login_response(login, uniqueness);
    return begin(session, now_us + 100U, 200U, 500U) &&
           d1l_meshcore_admin_accept_login_response(
               session, replay, PEER, login, sizeof(login), now_us) ==
               D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED;
}

static int test_allowlisted_mutations(void)
{
    CHECK(strcmp(d1l_meshcore_admin_mutation_name(
                     D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS),
                 "clear_stats") == 0);
    CHECK(strcmp(d1l_meshcore_admin_mutation_command(
                     D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS),
                 "clear stats") == 0);
    CHECK(strcmp(d1l_meshcore_admin_mutation_name(
                     D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP),
                 "advertise_zero_hop") == 0);
    CHECK(strcmp(d1l_meshcore_admin_mutation_command(
                     D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP),
                 "advert.zerohop") == 0);
    CHECK(d1l_meshcore_admin_mutation_command(
              D1L_MESHCORE_ADMIN_MUTATION_NONE) == NULL);

    d1l_meshcore_admin_session_t session = {0};
    d1l_meshcore_admin_replay_cache_t replay = {0};
    CHECK(authenticate_repeater(&session, &replay, 0x41U, 100U));

    const uint32_t first_tag = 0x10203040U;
    CHECK(d1l_meshcore_admin_begin_mutation(
        &session, D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS, first_tag,
        120U, 200U));
    static const uint8_t unrelated[] = "OK";
    CHECK(d1l_meshcore_admin_accept_mutation_response(
              &session, OTHER_PEER, 0x55667789U, unrelated,
              sizeof(unrelated) - 1U, 130U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED);
    CHECK(d1l_meshcore_admin_accept_mutation_response(
              &session, PEER, 0x55667789U, unrelated,
              sizeof(unrelated) - 1U, 130U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED);
    static const uint8_t clear_ok[] = "(OK - stats reset)";
    CHECK(d1l_meshcore_admin_accept_mutation_response(
              &session, PEER, 0x55667789U, clear_ok,
              sizeof(clear_ok) - 1U, 140U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.last_mutation == D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS);
    CHECK(session.last_mutation_success);
    CHECK(session.last_completed_tag == first_tag);

    CHECK(d1l_meshcore_admin_begin_mutation(
        &session, D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP,
        first_tag + 1U, 150U, 230U));
    static const uint8_t rejected[] = "ERR denied";
    CHECK(d1l_meshcore_admin_accept_mutation_response(
              &session, PEER, 0x5566778AU, rejected,
              sizeof(rejected) - 1U, 160U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_REJECTED);
    CHECK(session.last_mutation ==
          D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP);
    CHECK(!session.last_mutation_success);

    CHECK(d1l_meshcore_admin_begin_mutation(
        &session, D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP,
        first_tag + 2U, 170U, 240U));
    CHECK(d1l_meshcore_admin_cancel_mutation(
        &session, D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP,
        first_tag + 2U));
    CHECK(session.state == D1L_MESHCORE_ADMIN_AUTHENTICATED);

    session.permissions = 0U;
    CHECK(!d1l_meshcore_admin_begin_mutation(
        &session, D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS,
        first_tag + 3U, 180U, 250U));
    session.permissions = D1L_MESHCORE_ADMIN_PERMISSION_ADMIN;
    session.firmware_level = 1U;
    CHECK(!d1l_meshcore_admin_begin_mutation(
        &session, D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS,
        first_tag + 3U, 180U, 250U));
    return 0;
}

static int test_guest_sessions_are_read_only(void)
{
    d1l_meshcore_admin_session_t repeater = {0};
    d1l_meshcore_admin_replay_cache_t replay = {0};
    uint8_t login[16];
    login_response(login, 0x49U);
    login[6] = 0U;
    login[7] = D1L_MESHCORE_ADMIN_PERMISSION_GUEST;
    CHECK(begin(&repeater, 500U, 100U, 500U));
    CHECK(d1l_meshcore_admin_accept_login_response(
              &repeater, &replay, PEER, login, sizeof(login), 400U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(repeater.permissions == D1L_MESHCORE_ADMIN_PERMISSION_GUEST);
    CHECK(d1l_meshcore_admin_begin_status_request(
        &repeater, 0x01020311U, 410U, 470U));
    CHECK(d1l_meshcore_admin_cancel_status_request(
        &repeater, 0x01020311U));
    CHECK(!d1l_meshcore_admin_begin_mutation(
        &repeater, D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS,
        0x01020312U, 420U, 480U));
    CHECK(!d1l_meshcore_admin_begin_cli_command(
        &repeater, 0x01020313U, false, false,
        D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT, 420U, 480U));

    d1l_meshcore_admin_session_t room = {0};
    CHECK(d1l_meshcore_admin_begin_login(
        &room, D1L_MESHCORE_ADMIN_ROLE_ROOM, PEER, LOCAL, SECRET,
        700U, 100U, 500U));
    login_response_for_role(login, 0x4AU, 1U);
    login[6] = 0U;
    login[7] = D1L_MESHCORE_ADMIN_PERMISSION_WRITE;
    CHECK(d1l_meshcore_admin_accept_login_response(
              &room, &replay, PEER, login, sizeof(login), 600U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(room.permissions == D1L_MESHCORE_ADMIN_PERMISSION_WRITE);
    CHECK(!d1l_meshcore_admin_begin_cli_command(
        &room, 0x01020314U, false, false,
        D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT, 610U, 680U));
    CHECK(d1l_meshcore_admin_note_authenticated_activity(&room, 620U));
    CHECK(room.idle_deadline_us == 720U);
    return 0;
}

static int test_server_queries_and_protocol_permissions(void)
{
    uint8_t request[D1L_MESHCORE_ADMIN_MAX_QUERY_REQUEST_BYTES] = {0};
    size_t request_len = 0U;
    CHECK(d1l_meshcore_admin_encode_query_request(
        D1L_MESHCORE_ADMIN_QUERY_TELEMETRY, 0x01020304U, 0U,
        0xA1A2A3A4U, request, sizeof(request), &request_len));
    static const uint8_t expected_telemetry[] = {
        0x04U, 0x03U, 0x02U, 0x01U, 0x03U, 0U, 0U, 0U, 0U,
        0xA4U, 0xA3U, 0xA2U, 0xA1U};
    CHECK(request_len == sizeof(expected_telemetry));
    CHECK(memcmp(request, expected_telemetry,
                 sizeof(expected_telemetry)) == 0);

    CHECK(d1l_meshcore_admin_encode_query_request(
        D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST, 0x01020304U, 0U,
        0xA1A2A3A4U, request, sizeof(request), &request_len));
    static const uint8_t expected_access[] = {
        0x04U, 0x03U, 0x02U, 0x01U, 0x05U, 0U, 0U,
        0xA4U, 0xA3U, 0xA2U, 0xA1U};
    CHECK(request_len == sizeof(expected_access));
    CHECK(memcmp(request, expected_access, sizeof(expected_access)) == 0);

    CHECK(d1l_meshcore_admin_encode_query_request(
        D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS, 0x01020304U, 0x1234U,
        0xA1A2A3A4U, request, sizeof(request), &request_len));
    static const uint8_t expected_neighbours[] = {
        0x04U, 0x03U, 0x02U, 0x01U, 0x06U, 0U, 10U,
        0x34U, 0x12U, 0U, 4U, 0xA4U, 0xA3U, 0xA2U, 0xA1U};
    CHECK(request_len == sizeof(expected_neighbours));
    CHECK(memcmp(request, expected_neighbours,
                 sizeof(expected_neighbours)) == 0);
    CHECK(!d1l_meshcore_admin_encode_query_request(
        D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST, 0x01020304U, 1U,
        0xA1A2A3A4U, request, sizeof(request), &request_len));

    d1l_meshcore_admin_session_t session = {0};
    d1l_meshcore_admin_replay_cache_t replay = {0};
    CHECK(authenticate_repeater(&session, &replay, 0x61U, 100U));
    CHECK(d1l_meshcore_admin_query_allowed(
        D1L_MESHCORE_ADMIN_ROLE_REPEATER,
        D1L_MESHCORE_ADMIN_PERMISSION_GUEST,
        D1L_MESHCORE_ADMIN_QUERY_TELEMETRY));
    CHECK(d1l_meshcore_admin_query_allowed(
        D1L_MESHCORE_ADMIN_ROLE_REPEATER,
        D1L_MESHCORE_ADMIN_PERMISSION_GUEST,
        D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS));
    CHECK(!d1l_meshcore_admin_query_allowed(
        D1L_MESHCORE_ADMIN_ROLE_REPEATER,
        D1L_MESHCORE_ADMIN_PERMISSION_READ_ONLY,
        D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST));
    CHECK(!d1l_meshcore_admin_query_allowed(
        D1L_MESHCORE_ADMIN_ROLE_ROOM,
        D1L_MESHCORE_ADMIN_PERMISSION_ADMIN,
        D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS));

    const uint32_t telemetry_tag = 0x10203061U;
    CHECK(d1l_meshcore_admin_begin_query_request(
        &session, D1L_MESHCORE_ADMIN_QUERY_TELEMETRY, 0U,
        telemetry_tag, 120U, 200U));
    uint8_t telemetry[16] = {0};
    write_le32(telemetry, telemetry_tag);
    telemetry[4] = 1U;
    telemetry[5] = 116U;
    telemetry[6] = 0x01U;
    telemetry[7] = 0xA4U;
    telemetry[8] = 2U;
    telemetry[9] = 103U;
    telemetry[10] = 0xFFU;
    telemetry[11] = 0xC9U;
    telemetry[12] = 3U;
    telemetry[13] = 104U;
    telemetry[14] = 101U;
    CHECK(d1l_meshcore_admin_accept_query_response(
              &session, PEER, telemetry, sizeof(telemetry), 130U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.query_result.valid);
    CHECK(session.query_result.kind ==
          D1L_MESHCORE_ADMIN_QUERY_TELEMETRY);
    CHECK(session.query_result.count == 3U);
    CHECK(strstr(session.query_result.text, "voltage 4.20 V") != NULL);
    CHECK(strstr(session.query_result.text, "temperature -5.5 C") != NULL);
    CHECK(strstr(session.query_result.text, "humidity 50.5 %") != NULL);

    const uint32_t access_tag = telemetry_tag + 1U;
    CHECK(d1l_meshcore_admin_begin_query_request(
        &session, D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST, 0U,
        access_tag, 140U, 220U));
    uint8_t access[16] = {0};
    write_le32(access, access_tag);
    static const uint8_t access_entry[] = {
        0xA1U, 0xB2U, 0xC3U, 0xD4U, 0xE5U, 0xF6U,
        D1L_MESHCORE_ADMIN_PERMISSION_WRITE};
    memcpy(&access[4], access_entry, sizeof(access_entry));
    CHECK(d1l_meshcore_admin_accept_query_response(
              &session, PEER, access, sizeof(access), 150U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.query_result.count == 1U);
    CHECK(strstr(
              session.query_result.text,
              "A1B2C3D4E5F6  read-write") != NULL);

    const uint32_t neighbours_tag = access_tag + 1U;
    CHECK(d1l_meshcore_admin_begin_query_request(
        &session, D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS, 0U,
        neighbours_tag, 160U, 240U));
    uint8_t neighbours[32] = {0};
    write_le32(neighbours, neighbours_tag);
    write_le16(&neighbours[4], 12U);
    write_le16(&neighbours[6], 2U);
    neighbours[8] = 0x01U;
    neighbours[9] = 0x02U;
    neighbours[10] = 0x03U;
    neighbours[11] = 0x04U;
    write_le32(&neighbours[12], 90U);
    neighbours[16] = (uint8_t)-5;
    neighbours[17] = 0xA0U;
    neighbours[18] = 0xB0U;
    neighbours[19] = 0xC0U;
    neighbours[20] = 0xD0U;
    write_le32(&neighbours[21], 4U);
    neighbours[25] = 7U;
    CHECK(d1l_meshcore_admin_accept_query_response(
              &session, PEER, neighbours, sizeof(neighbours), 170U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.query_result.offset == 0U);
    CHECK(session.query_result.total == 12U);
    CHECK(session.query_result.count == 2U);
    CHECK(strstr(session.query_result.text, "-1.25 dB") != NULL);
    CHECK(strstr(session.query_result.text, "1.75 dB") != NULL);

    d1l_meshcore_admin_session_t guest_repeater = {0};
    uint8_t login[16];
    CHECK(begin(&guest_repeater, 400U, 100U, 300U));
    login_response(login, 0x62U);
    login[6] = 0U;
    login[7] = D1L_MESHCORE_ADMIN_PERMISSION_GUEST;
    CHECK(d1l_meshcore_admin_accept_login_response(
              &guest_repeater, &replay, PEER, login, sizeof(login), 300U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);

    d1l_meshcore_admin_session_t guest_room = {0};
    CHECK(d1l_meshcore_admin_begin_login(
        &guest_room, D1L_MESHCORE_ADMIN_ROLE_ROOM, PEER, LOCAL, SECRET,
        600U, 100U, 300U));
    login_response_for_role(login, 0x63U, 1U);
    login[6] = 2U;
    login[7] = D1L_MESHCORE_ADMIN_PERMISSION_GUEST;
    CHECK(d1l_meshcore_admin_accept_login_response(
              &guest_room, &replay, PEER, login, sizeof(login), 500U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    return 0;
}

static int test_bounded_cli_session_and_redaction(void)
{
    CHECK(d1l_meshcore_admin_cli_command_valid("neighbors"));
    CHECK(d1l_meshcore_admin_cli_command_read_only("neighbors"));
    CHECK(!d1l_meshcore_admin_cli_command_valid("NEIGHBORS"));
    CHECK(d1l_meshcore_admin_cli_command_read_only("get name"));
    CHECK(d1l_meshcore_admin_cli_command_read_only("region list allowed"));
    CHECK(d1l_meshcore_admin_cli_command_read_only("region get home"));
    CHECK(!d1l_meshcore_admin_cli_command_read_only("region put test"));
    CHECK(!d1l_meshcore_admin_cli_command_read_only("set name repeater"));
    CHECK(d1l_meshcore_admin_cli_command_policy("set name repeater") ==
          D1L_MESHCORE_ADMIN_CLI_MUTATION);
    CHECK(d1l_meshcore_admin_cli_command_sensitive(
        "set guest.password secret"));
    CHECK(!d1l_meshcore_admin_cli_command_valid(
        "PASSWORD new-secret"));
    CHECK(d1l_meshcore_admin_cli_command_sensitive(
        "password new-secret"));
    CHECK(d1l_meshcore_admin_cli_command_sensitive(
        "set bridge.secret secret"));
    CHECK(!d1l_meshcore_admin_cli_command_read_only(
        "get guest.password"));
    CHECK(!d1l_meshcore_admin_cli_command_valid("help"));
    CHECK(!d1l_meshcore_admin_cli_command_valid("reboot"));
    CHECK(!d1l_meshcore_admin_cli_command_valid("start ota"));
    CHECK(!d1l_meshcore_admin_cli_command_valid("get prv.key"));
    CHECK(!d1l_meshcore_admin_cli_command_valid("set freq 915.0"));
    CHECK(d1l_meshcore_admin_cli_command_read_only("get freq"));
    CHECK(d1l_meshcore_admin_cli_command_allowed(
        "neighbors", D1L_MESHCORE_ADMIN_ROLE_REPEATER,
        D1L_MESHCORE_ADMIN_PERMISSION_ADMIN));
    CHECK(!d1l_meshcore_admin_cli_command_allowed(
        "neighbors", D1L_MESHCORE_ADMIN_ROLE_ROOM,
        D1L_MESHCORE_ADMIN_PERMISSION_ADMIN));
    CHECK(d1l_meshcore_admin_cli_command_allowed(
        "get allow.read.only", D1L_MESHCORE_ADMIN_ROLE_ROOM,
        D1L_MESHCORE_ADMIN_PERMISSION_ADMIN));
    CHECK(!d1l_meshcore_admin_cli_command_allowed(
        "get allow.read.only", D1L_MESHCORE_ADMIN_ROLE_REPEATER,
        D1L_MESHCORE_ADMIN_PERMISSION_ADMIN));
    CHECK(!d1l_meshcore_admin_cli_command_allowed(
        "ver", D1L_MESHCORE_ADMIN_ROLE_REPEATER,
        D1L_MESHCORE_ADMIN_PERMISSION_READ_ONLY));
    static const char acl_key[] =
        "00112233445566778899aabbccddeeff"
        "00112233445566778899aabbccddeeff";
    char acl_command[D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES + 1U] = {0};
    CHECK(d1l_meshcore_admin_format_acl_command(
        acl_key, D1L_MESHCORE_ADMIN_PERMISSION_WRITE,
        acl_command, sizeof(acl_command)));
    CHECK(strcmp(
        acl_command,
        "setperm 00112233445566778899aabbccddeeff"
        "00112233445566778899aabbccddeeff 2") == 0);
    CHECK(d1l_meshcore_admin_cli_command_policy(acl_command) ==
          D1L_MESHCORE_ADMIN_CLI_MUTATION);
    CHECK(!d1l_meshcore_admin_format_acl_command(
        "0011", D1L_MESHCORE_ADMIN_PERMISSION_WRITE,
        acl_command, sizeof(acl_command)));
    CHECK(!d1l_meshcore_admin_format_acl_command(
        acl_key, 4U, acl_command, sizeof(acl_command)));
    CHECK(!d1l_meshcore_admin_cli_command_valid(" neighbors"));
    CHECK(!d1l_meshcore_admin_cli_command_valid("neighbors\n"));
    CHECK(d1l_meshcore_admin_cli_command_reply_profile(
              "get bootloader.ver") ==
          D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNKNOWN_VALUE);
    CHECK(d1l_meshcore_admin_cli_command_reply_profile(
              "get pwrmgt.support") ==
          D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNSUPPORTED_VALUE);
    CHECK(d1l_meshcore_admin_cli_command_reply_profile("get name") ==
          D1L_MESHCORE_ADMIN_CLI_REPLY_GET_VALUE);
    CHECK(d1l_meshcore_admin_cli_command_reply_profile(
              "get adc.multiplier") ==
          D1L_MESHCORE_ADMIN_CLI_REPLY_ADC_UNSUPPORTED);
    CHECK(d1l_meshcore_admin_cli_command_reply_profile(
              "get pwrmgt.source") ==
          D1L_MESHCORE_ADMIN_CLI_REPLY_POWER_MANAGEMENT_UNSUPPORTED);
    CHECK(d1l_meshcore_admin_cli_command_reply_profile("gps") ==
          D1L_MESHCORE_ADMIN_CLI_REPLY_GPS_NOT_FOUND);
    CHECK(d1l_meshcore_admin_cli_command_reply_profile("gps advert") ==
          D1L_MESHCORE_ADMIN_CLI_REPLY_GPS_ADVERT_ERROR);
    CHECK(d1l_meshcore_admin_cli_command_reply_profile(
              "region get Waterloo") ==
          D1L_MESHCORE_ADMIN_CLI_REPLY_REGION_NOT_FOUND);
    CHECK(d1l_meshcore_admin_cli_command_reply_profile("neighbors") ==
          D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT);

    d1l_meshcore_admin_session_t session = {0};
    d1l_meshcore_admin_replay_cache_t replay = {0};
    CHECK(authenticate_repeater(&session, &replay, 0x51U, 100U));
    const uint32_t tag = 0x10203050U;
    CHECK(d1l_meshcore_admin_begin_cli_command(
        &session, tag, false, true,
        D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT, 120U, 200U));
    static const uint8_t reply[] = "2 neighbors\nA1B2";
    CHECK(d1l_meshcore_admin_accept_cli_response(
              &session, PEER, 0x55667789U, reply, sizeof(reply) - 1U,
              130U) == D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.state == D1L_MESHCORE_ADMIN_AUTHENTICATED);
    CHECK(session.cli_reply_valid);
    CHECK(session.cli_reply_success);
    CHECK(!session.cli_reply_redacted);
    CHECK(strcmp(session.cli_reply, (const char *)reply) == 0);

    CHECK(d1l_meshcore_admin_begin_cli_command(
        &session, tag + 1U, true, false,
        D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT, 140U, 210U));
    static const uint8_t sensitive_reply[] = "password now: secret";
    CHECK(d1l_meshcore_admin_accept_cli_response(
              &session, PEER, 0x5566778AU, sensitive_reply,
              sizeof(sensitive_reply) - 1U, 150U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.cli_reply_valid);
    CHECK(session.cli_reply_redacted);
    CHECK(strstr(session.cli_reply, "secret") == NULL);
    CHECK(strcmp(session.cli_reply, "[sensitive response hidden]") == 0);

    CHECK(d1l_meshcore_admin_begin_cli_command(
        &session, tag + 2U, false, false,
        D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT, 160U, 220U));
    static const uint8_t error_reply[] = "Unknown command";
    CHECK(d1l_meshcore_admin_accept_cli_response(
              &session, PEER, 0x5566778BU, error_reply,
              sizeof(error_reply) - 1U, 170U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_REJECTED);
    CHECK(!session.cli_reply_success);
    CHECK(d1l_meshcore_admin_begin_cli_command(
        &session, tag + 3U, false, false,
        D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT, 175U, 225U));
    static const uint8_t config_error_reply[] = "unknown config: bogus";
    CHECK(d1l_meshcore_admin_accept_cli_response(
              &session, PEER, 0x5566778CU, config_error_reply,
              sizeof(config_error_reply) - 1U, 176U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_REJECTED);
    CHECK(!session.cli_reply_success);
    CHECK(d1l_meshcore_admin_begin_cli_command(
        &session, tag + 4U, false, true,
        D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT, 180U, 230U));
    static const uint8_t unsupported_value[] = "> unsupported";
    CHECK(d1l_meshcore_admin_accept_cli_response(
              &session, PEER, 0x5566778DU, unsupported_value,
              sizeof(unsupported_value) - 1U, 181U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.cli_reply_success);

    CHECK(d1l_meshcore_admin_begin_cli_command(
        &session, tag + 5U, false, true,
        D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNSUPPORTED_VALUE,
        182U, 240U));
    CHECK(d1l_meshcore_admin_accept_cli_response(
              &session, PEER, 0x5566778EU, unsupported_value,
              sizeof(unsupported_value) - 1U, 183U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.cli_reply_success);

    static const uint8_t unknown_value[] = "> unknown";
    CHECK(d1l_meshcore_admin_begin_cli_command(
        &session, tag + 6U, false, true,
        D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNKNOWN_VALUE,
        184U, 250U));
    CHECK(d1l_meshcore_admin_accept_cli_response(
              &session, PEER, 0x5566778FU, unknown_value,
              sizeof(unknown_value) - 1U, 185U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);

    static const struct {
        const char *text;
        d1l_meshcore_admin_cli_reply_profile_t profile;
    } user_controlled_read_only_values[] = {
        {"> Unknown Valley", D1L_MESHCORE_ADMIN_CLI_REPLY_GET_VALUE},
        {"> owner.info: error handling is unsupported / not supported",
         D1L_MESHCORE_ADMIN_CLI_REPLY_GET_VALUE},
        {"Unknown Valley", D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT},
        {"Error: Valley", D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT},
        {"> ERR: unsupported", D1L_MESHCORE_ADMIN_CLI_REPLY_GET_VALUE},
    };
    uint32_t value_tag = tag + 7U;
    uint32_t value_timestamp = 0x55667790U;
    for (size_t i = 0U;
         i < sizeof(user_controlled_read_only_values) /
                 sizeof(user_controlled_read_only_values[0]); ++i) {
        CHECK(d1l_meshcore_admin_begin_cli_command(
            &session, value_tag++, false, true,
            user_controlled_read_only_values[i].profile,
            186U + i, 270U + i));
        CHECK(d1l_meshcore_admin_accept_cli_response(
                  &session, PEER, value_timestamp++,
                  (const uint8_t *)user_controlled_read_only_values[i].text,
                  strlen(user_controlled_read_only_values[i].text),
                  187U + i) ==
              D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
        CHECK(session.cli_reply_success);
    }

    static const struct {
        const char *text;
        d1l_meshcore_admin_cli_reply_profile_t profile;
    } exact_read_only_failures[] = {
        {"Unknown command", D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT},
        {"ERROR: unsupported",
         D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNKNOWN_VALUE},
        {"??: name", D1L_MESHCORE_ADMIN_CLI_REPLY_GET_VALUE},
        {"Error: unsupported by this board",
         D1L_MESHCORE_ADMIN_CLI_REPLY_ADC_UNSUPPORTED},
        {"ERROR: Power management not supported",
         D1L_MESHCORE_ADMIN_CLI_REPLY_POWER_MANAGEMENT_UNSUPPORTED},
        {"Can't find GPS", D1L_MESHCORE_ADMIN_CLI_REPLY_GPS_NOT_FOUND},
        {"error", D1L_MESHCORE_ADMIN_CLI_REPLY_GPS_ADVERT_ERROR},
        {"Err - unknown region",
         D1L_MESHCORE_ADMIN_CLI_REPLY_REGION_NOT_FOUND},
    };
    uint32_t failure_tag = value_tag;
    uint32_t failure_timestamp = value_timestamp;
    for (size_t i = 0U;
         i < sizeof(exact_read_only_failures) /
                 sizeof(exact_read_only_failures[0]); ++i) {
        CHECK(d1l_meshcore_admin_begin_cli_command(
            &session, failure_tag++, false, true,
            exact_read_only_failures[i].profile,
            194U + i, 290U + i));
        CHECK(d1l_meshcore_admin_accept_cli_response(
                  &session, PEER, failure_timestamp++,
                  (const uint8_t *)exact_read_only_failures[i].text,
                  strlen(exact_read_only_failures[i].text), 195U + i) ==
              D1L_MESHCORE_ADMIN_RESPONSE_REJECTED);
        CHECK(!session.cli_reply_success);
    }

    static const char *const pinned_failures[] = {
        "  (ERR: clock cannot go backwards)",
        "gps toggle not found",
        "gps provider not found",
        "Bridge not supported",
        "Board not supported",
        "??: bogus",
        "Error, invalid params",
        "Err - unable to put",
    };
    for (size_t i = 0U;
         i < sizeof(pinned_failures) / sizeof(pinned_failures[0]); ++i) {
        CHECK(d1l_meshcore_admin_begin_cli_command(
            &session, failure_tag++, false, false,
            D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT,
            210U + i, 330U + i));
        CHECK(d1l_meshcore_admin_accept_cli_response(
                  &session, PEER, failure_timestamp++,
                  (const uint8_t *)pinned_failures[i],
                  strlen(pinned_failures[i]), 211U + i) ==
              D1L_MESHCORE_ADMIN_RESPONSE_REJECTED);
        CHECK(!session.cli_reply_success);
    }

    CHECK(d1l_meshcore_admin_begin_cli_command(
        &session, failure_tag, false, false,
        D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT, 230U, 400U));
    CHECK(d1l_meshcore_admin_cancel_cli_command(
        &session, failure_tag));
    CHECK(session.state == D1L_MESHCORE_ADMIN_AUTHENTICATED);
    return 0;
}

static int test_replay_capacity_and_deterministic_eviction(void)
{
    d1l_meshcore_admin_session_t session = {0};
    d1l_meshcore_admin_replay_cache_t replay = {0};
    uint8_t login[16];
    for (uint8_t index = 0U;
         index < D1L_MESHCORE_ADMIN_REPLAY_RESPONSES_PER_PEER + 1U;
         ++index) {
        const uint64_t now = 100U + (uint64_t)index * 100U;
        CHECK(begin(&session, now + 50U, 100U, 300U));
        login_response(login, (uint8_t)(0x30U + index));
        CHECK(d1l_meshcore_admin_accept_login_response(
                  &session, &replay, PEER, login, sizeof(login), now) ==
              D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
        d1l_meshcore_admin_reset(&session);
    }
    CHECK(replay.peers[0].response_count ==
          D1L_MESHCORE_ADMIN_REPLAY_RESPONSES_PER_PEER);
    CHECK(replay.peers[0].next_response == 1U);

    /* The fifth response evicts the first response bytes, but the per-peer
     * server timestamp high-water must still reject that captured success. */
    CHECK(begin(&session, 700U, 100U, 300U));
    login_response(login, 0x30U);
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, PEER, login, sizeof(login), 650U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_REPLAYED);
    CHECK(session.state ==
          D1L_MESHCORE_ADMIN_VOLATILE_REPLAY_REJECTED);
    CHECK(replay.peers[0].next_response == 1U);
    d1l_meshcore_admin_reset(&session);
    CHECK(begin(&session, 800U, 100U, 300U));
    login_response(login, 0x32U);
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, PEER, login, sizeof(login), 750U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_REPLAYED);
    CHECK(session.state ==
          D1L_MESHCORE_ADMIN_VOLATILE_REPLAY_REJECTED);
    return 0;
}

static int test_truthful_login_failure_states(void)
{
    d1l_meshcore_admin_session_t session = {0};
    d1l_meshcore_admin_replay_cache_t replay = {0};
    uint8_t login[16];

    CHECK(begin(&session, 200U, 100U, 300U));
    login_response(login, 0x60U);
    login[4] = 1U;
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, PEER, login, sizeof(login), 100U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_REJECTED);
    CHECK(session.state == D1L_MESHCORE_ADMIN_REJECTED_CREDENTIALS);
    CHECK(all_zero(session.session_secret, sizeof(session.session_secret)));

    CHECK(begin(&session, 400U, 100U, 300U));
    login_response(login, 0x61U);
    login[12] = 9U;
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, PEER, login, sizeof(login), 300U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED);
    CHECK(session.state == D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL);

    CHECK(begin(&session, 600U, 100U, 300U));
    CHECK(d1l_meshcore_admin_fail(
        &session, D1L_MESHCORE_ADMIN_RADIO_BUSY));
    CHECK(session.state == D1L_MESHCORE_ADMIN_RADIO_BUSY);
    CHECK(session.role == D1L_MESHCORE_ADMIN_ROLE_REPEATER);
    CHECK(all_zero(session.session_secret, sizeof(session.session_secret)));
    CHECK(begin(&session, 800U, 100U, 300U));
    CHECK(d1l_meshcore_admin_fail(
        &session, D1L_MESHCORE_ADMIN_DISCONNECTED));
    CHECK(session.state == D1L_MESHCORE_ADMIN_DISCONNECTED);
    CHECK(!d1l_meshcore_admin_fail(
        &session, D1L_MESHCORE_ADMIN_AUTHENTICATED));
    return 0;
}

int main(void)
{
    CHECK(test_codec_and_repeater_status() == 0);
    CHECK(test_exact_deadlines_and_zeroization() == 0);
    CHECK(test_login_replay_and_room_no_history() == 0);
    CHECK(test_allowlisted_mutations() == 0);
    CHECK(test_guest_sessions_are_read_only() == 0);
    CHECK(test_server_queries_and_protocol_permissions() == 0);
    CHECK(test_bounded_cli_session_and_redaction() == 0);
    CHECK(test_replay_capacity_and_deterministic_eviction() == 0);
    CHECK(test_truthful_login_failure_states() == 0);
    puts("meshcore_admin_dispatch_test: ok");
    return 0;
}
