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
    return entry && strcmp(entry->type, "room") == 0;
}

static uint8_t captured_plaintext[D1L_MESHCORE_ADMIN_MAX_LOGIN_REQUEST_BYTES];
static size_t captured_plaintext_len;

static void write_le32(uint8_t *dest, uint32_t value)
{
    dest[0] = (uint8_t)value;
    dest[1] = (uint8_t)(value >> 8U);
    dest[2] = (uint8_t)(value >> 16U);
    dest[3] = (uint8_t)(value >> 24U);
}

static esp_err_t derive_secret(
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t local_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    uint8_t out_secret[D1L_MESHCORE_ADMIN_SECRET_BYTES])
{
    if (!peer_public_key || !local_public_key || !out_secret) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < D1L_MESHCORE_ADMIN_SECRET_BYTES; ++i) {
        out_secret[i] = (uint8_t)(peer_public_key[i] ^ local_public_key[i]);
    }
    return ESP_OK;
}

static esp_err_t capture_encrypt(
    const uint8_t secret[D1L_MESHCORE_ADMIN_SECRET_BYTES], uint8_t *dest,
    size_t dest_size, const uint8_t *src, size_t src_len, size_t *out_len)
{
    if (!secret || !dest || !src || !out_len ||
        dest_size < src_len || sizeof(captured_plaintext) < src_len) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(captured_plaintext, src, src_len);
    captured_plaintext_len = src_len;
    memcpy(dest, src, src_len);
    *out_len = src_len;
    return ESP_OK;
}

static int test_empty_room_login_negotiates_guest_permissions(void)
{
    static const char public_key_hex[] =
        "00112233445566778899aabbccddeeff"
        "00112233445566778899aabbccddeeff";
    const uint32_t timestamp = UINT32_C(0x10203040);
    d1l_settings_t settings = {
        .identity_ready = true,
    };
    d1l_contact_entry_t contact = {0};
    d1l_meshcore_route_selection_t selection = {
        .route = D1L_MESHCORE_ROUTE_FLOOD,
        .path_len = 0U,
        .path_byte_len = 0U,
        .path_hash_bytes = 1U,
        .path_hops = 0U,
    };
    d1l_meshcore_admin_binding_t binding = {0};
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
    uint8_t raw_len = 0U;

    for (size_t i = 0U; i < sizeof(settings.identity_public_key); ++i) {
        settings.identity_public_key[i] = (uint8_t)(0x80U + i);
    }
    snprintf(contact.fingerprint, sizeof(contact.fingerprint), "ROOMEMPTY");
    snprintf(contact.public_key_hex, sizeof(contact.public_key_hex), "%s",
             public_key_hex);
    snprintf(contact.type, sizeof(contact.type), "room");
    memset(captured_plaintext, 0, sizeof(captured_plaintext));
    captured_plaintext_len = 0U;

    CHECK(d1l_meshcore_admin_build_login_packet(
              &settings, &contact, &selection, "", timestamp,
              derive_secret, capture_encrypt, &binding, raw, sizeof(raw),
              &raw_len) == ESP_OK);
    CHECK(raw_len > 0U);
    CHECK(binding.role == D1L_MESHCORE_ADMIN_ROLE_ROOM);
    CHECK(captured_plaintext_len ==
          D1L_MESHCORE_ADMIN_ROOM_LOGIN_PREFIX_BYTES);
    CHECK(captured_plaintext[0] == 0x40U);
    CHECK(captured_plaintext[1] == 0x30U);
    CHECK(captured_plaintext[2] == 0x20U);
    CHECK(captured_plaintext[3] == 0x10U);
    CHECK(memcmp(captured_plaintext, &captured_plaintext[4], 4U) == 0);

    d1l_meshcore_admin_session_t session = {0};
    d1l_meshcore_admin_replay_cache_t replay = {0};
    uint8_t response[D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES] = {0};
    write_le32(response, timestamp + 1U);
    response[6] = 2U;
    response[7] = D1L_MESHCORE_ADMIN_PERMISSION_GUEST;
    response[8] = 1U;
    response[12] = 1U;
    CHECK(d1l_meshcore_admin_begin_login(
        &session, binding.role, binding.peer_public_key,
        binding.local_public_key, binding.session_secret,
        100U, 100U, 500U));
    CHECK(d1l_meshcore_admin_accept_login_response(
              &session, &replay, binding.peer_public_key, response,
              sizeof(response), 10U) ==
          D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED);
    CHECK(session.permissions == D1L_MESHCORE_ADMIN_PERMISSION_GUEST);
    CHECK(d1l_meshcore_admin_begin_status_request(
        &session, 1U, 20U, 30U));
    CHECK(d1l_meshcore_admin_cancel_status_request(&session, 1U));
    CHECK(d1l_meshcore_admin_begin_query_request(
        &session, D1L_MESHCORE_ADMIN_QUERY_TELEMETRY, 0U,
        2U, 20U, 30U));
    CHECK(d1l_meshcore_admin_cancel_query_request(
        &session, D1L_MESHCORE_ADMIN_QUERY_TELEMETRY, 2U));
    CHECK(!d1l_meshcore_admin_begin_query_request(
        &session, D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST, 0U,
        3U, 20U, 30U));
    CHECK(!d1l_meshcore_admin_begin_mutation(
        &session, D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS,
        4U, 20U, 30U));
    CHECK(!d1l_meshcore_admin_begin_cli_command(
        &session, 5U, false, true, D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT,
        20U, 30U));
    return 0;
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
    CHECK(test_empty_room_login_negotiates_guest_permissions() == 0);
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

    /* The in-memory high-water produces a distinct, externally visible
     * rejection before any durable-store lookup is needed. */
    CHECK(dispatch_login(&binding, response, &result));
    CHECK(result == D1L_MESHCORE_ADMIN_RESPONSE_REPLAYED);
    d1l_meshcore_admin_runtime_snapshot(&snapshot);
    CHECK(snapshot.state ==
          D1L_MESHCORE_ADMIN_VOLATILE_REPLAY_REJECTED);

    /* Runtime init models a local reboot: volatile rings are empty, but the
     * committed per-full-key server timestamp high-water remains. */
    d1l_meshcore_admin_runtime_init();
    CHECK(dispatch_login(&binding, response, &result));
    CHECK(result == D1L_MESHCORE_ADMIN_RESPONSE_REPLAYED);
    d1l_meshcore_admin_runtime_snapshot(&snapshot);
    CHECK(snapshot.state ==
          D1L_MESHCORE_ADMIN_DURABLE_REPLAY_REJECTED);

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
    mock_nvs_fail_next_get(ESP_FAIL);
    CHECK(dispatch_login(&binding, response, &result));
    CHECK(result == D1L_MESHCORE_ADMIN_RESPONSE_REJECTED);
    d1l_meshcore_admin_runtime_snapshot(&snapshot);
    CHECK(snapshot.state == D1L_MESHCORE_ADMIN_LOCAL_STORAGE_FAILED);
    CHECK(snapshot.last_error == ESP_FAIL);

    make_login_response(response, 0x5566778BU, 4U);
    mock_nvs_fail_next_commit(ESP_FAIL);
    CHECK(dispatch_login(&binding, response, &result));
    CHECK(result == D1L_MESHCORE_ADMIN_RESPONSE_REJECTED);
    d1l_meshcore_admin_runtime_snapshot(&snapshot);
    CHECK(snapshot.state == D1L_MESHCORE_ADMIN_LOCAL_STORAGE_FAILED);
    CHECK(snapshot.last_error == ESP_FAIL);
    puts("meshcore_admin_runtime_replay_test: ok");
    return 0;
}
