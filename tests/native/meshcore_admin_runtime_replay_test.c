#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mesh/meshcore_admin_runtime.h"
#include "mock_esp_nvs.h"

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,          \
                    __LINE__, #condition);                                      \
            return 1;                                                           \
        }                                                                       \
    } while (0)

bool d1l_contact_store_can_admin(const d1l_contact_entry_t *entry)
{
    (void)entry;
    return false;
}

static void write_le32(uint8_t *dest, uint32_t value)
{
    dest[0] = (uint8_t)value;
    dest[1] = (uint8_t)(value >> 8U);
    dest[2] = (uint8_t)(value >> 16U);
    dest[3] = (uint8_t)(value >> 24U);
}

static void make_binding(d1l_meshcore_admin_binding_t *binding)
{
    memset(binding, 0, sizeof(*binding));
    snprintf(binding->fingerprint, sizeof(binding->fingerprint), "DURABLE01");
    binding->role = D1L_MESHCORE_ADMIN_ROLE_REPEATER;
    for (size_t i = 0U; i < D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES; ++i) {
        binding->peer_public_key[i] = (uint8_t)(0x30U + i);
        binding->local_public_key[i] = (uint8_t)(0x60U + i);
        binding->session_secret[i] = (uint8_t)(0x90U + i);
    }
}

static void make_login_response(
    uint8_t response[D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES],
    uint32_t server_timestamp, uint8_t uniqueness)
{
    memset(response, 0, D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES);
    write_le32(response, server_timestamp);
    response[6] = 1U;
    response[7] = D1L_MESHCORE_ADMIN_PERMISSION_ADMIN;
    response[8] = uniqueness;
    response[9] = 0x20U;
    response[10] = 0x30U;
    response[11] = 0x40U;
    response[12] = 2U;
}

static bool dispatch_login(
    const d1l_meshcore_admin_binding_t *binding,
    const uint8_t response[D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES],
    d1l_meshcore_admin_response_result_t *out_result)
{
    if (!binding || !response || !out_result) {
        return false;
    }
    uint32_t generation = 0U;
    if (!d1l_meshcore_admin_runtime_begin_login(
            binding, 100U, &generation)) {
        return false;
    }
    bool considered = false;
    *out_result =
        d1l_meshcore_admin_runtime_dispatch_response(
            binding, generation, response,
            D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES, 110U, &considered);
    return considered;
}

int main(void)
{
    mock_nvs_reset();
    d1l_meshcore_admin_binding_t binding = {0};
    make_binding(&binding);
    uint8_t response[D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES] = {0};
    make_login_response(response, 0x55667788U, 1U);
    d1l_meshcore_admin_response_result_t result =
        D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;

    d1l_meshcore_admin_runtime_init();
    CHECK(dispatch_login(&binding, response, &result));
    CHECK(result == D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(mock_nvs_set_call_count() == 1U);
    CHECK(mock_nvs_commit_call_count() == 1U);

    d1l_meshcore_admin_runtime_snapshot_t snapshot = {0};
    d1l_meshcore_admin_runtime_snapshot(&snapshot);
    uint32_t request_generation = 0U;
    CHECK(d1l_meshcore_admin_runtime_begin_status(
        &binding, snapshot.generation, 0x10203040U, 120U,
        &request_generation));
    const uint8_t malformed[] = {0U};
    bool considered = false;
    CHECK(d1l_meshcore_admin_runtime_dispatch_response(
              &binding, request_generation, malformed, sizeof(malformed),
              130U, &considered) ==
          D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED);
    CHECK(considered);
    d1l_meshcore_admin_runtime_snapshot(&snapshot);
    CHECK(snapshot.state == D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL);

    /* Runtime init models a local reboot: volatile rings are empty, but the
     * committed per-full-key server timestamp high-water remains. */
    d1l_meshcore_admin_runtime_init();
    CHECK(dispatch_login(&binding, response, &result));
    CHECK(result == D1L_MESHCORE_ADMIN_RESPONSE_REPLAYED);
    d1l_meshcore_admin_runtime_snapshot(&snapshot);
    CHECK(snapshot.state == D1L_MESHCORE_ADMIN_TIMED_OUT);

    d1l_meshcore_admin_runtime_init();
    make_login_response(response, 0x55667789U, 2U);
    CHECK(dispatch_login(&binding, response, &result));
    CHECK(result == D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    d1l_meshcore_admin_runtime_snapshot(&snapshot);
    CHECK(d1l_meshcore_admin_runtime_begin_query(
        &binding, snapshot.generation,
        D1L_MESHCORE_ADMIN_QUERY_TELEMETRY, 0U, 0x10203041U, 140U,
        &request_generation));
    considered = false;
    CHECK(d1l_meshcore_admin_runtime_dispatch_response(
              &binding, request_generation, malformed, sizeof(malformed),
              150U, &considered) ==
          D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED);
    CHECK(considered);
    d1l_meshcore_admin_runtime_snapshot(&snapshot);
    CHECK(snapshot.state == D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL);

    d1l_meshcore_admin_runtime_init();
    make_login_response(response, 0x5566778AU, 3U);
    mock_nvs_fail_next_open(ESP_FAIL);
    CHECK(dispatch_login(&binding, response, &result));
    CHECK(result == D1L_MESHCORE_ADMIN_RESPONSE_REJECTED);
    d1l_meshcore_admin_runtime_snapshot(&snapshot);
    CHECK(snapshot.state == D1L_MESHCORE_ADMIN_DISCONNECTED);
    CHECK(snapshot.last_error == ESP_FAIL);
    puts("meshcore_admin_runtime_replay_test: ok");
    return 0;
}
