#include "meshcore_admin_dispatch.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint16_t read_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8U);
}

static int16_t read_le_i16(const uint8_t *src)
{
    return (int16_t)read_le16(src);
}

static uint32_t read_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8U) |
           ((uint32_t)src[2] << 16U) | ((uint32_t)src[3] << 24U);
}

static uint16_t read_be16(const uint8_t *src)
{
    return ((uint16_t)src[0] << 8U) | (uint16_t)src[1];
}

static int16_t read_be_i16(const uint8_t *src)
{
    return (int16_t)read_be16(src);
}

static uint32_t read_be32(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24U) | ((uint32_t)src[1] << 16U) |
           ((uint32_t)src[2] << 8U) | (uint32_t)src[3];
}

static int32_t read_be_i24(const uint8_t *src)
{
    uint32_t value = ((uint32_t)src[0] << 16U) |
                     ((uint32_t)src[1] << 8U) | (uint32_t)src[2];
    if ((value & UINT32_C(0x00800000)) != 0U) {
        value |= UINT32_C(0xFF000000);
    }
    return (int32_t)value;
}

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

static bool bytes_equal(const uint8_t *lhs, const uint8_t *rhs, size_t size)
{
    if (!lhs || !rhs) {
        return false;
    }
    uint8_t difference = 0U;
    for (size_t i = 0U; i < size; ++i) {
        difference |= (uint8_t)(lhs[i] ^ rhs[i]);
    }
    return difference == 0U;
}

static uint64_t deadline_after(uint64_t now_us, uint64_t timeout_us)
{
    return now_us > UINT64_MAX - timeout_us ? UINT64_MAX
                                             : now_us + timeout_us;
}

static bool deadline_due(uint64_t deadline_us, uint64_t now_us)
{
    return deadline_us != 0U && now_us >= deadline_us;
}

void d1l_meshcore_admin_secure_zero(void *value, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)value;
    while (bytes && size-- > 0U) {
        *bytes++ = 0U;
    }
}

bool d1l_meshcore_admin_encode_login_request(
    d1l_meshcore_admin_role_t role, uint32_t timestamp,
    uint32_t room_sync_since,
    const uint8_t *password, size_t password_len, uint8_t *out,
    size_t out_size, size_t *out_len)
{
    if (!out || !out_len || password_len > D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES ||
        (password_len > 0U && !password) ||
        (role != D1L_MESHCORE_ADMIN_ROLE_REPEATER &&
         role != D1L_MESHCORE_ADMIN_ROLE_ROOM) ||
        (role == D1L_MESHCORE_ADMIN_ROLE_REPEATER && room_sync_since != 0U)) {
        return false;
    }
    const size_t prefix_len =
        role == D1L_MESHCORE_ADMIN_ROLE_ROOM
            ? D1L_MESHCORE_ADMIN_ROOM_LOGIN_PREFIX_BYTES
            : D1L_MESHCORE_ADMIN_REPEATER_LOGIN_PREFIX_BYTES;
    if (out_size < prefix_len + password_len) {
        return false;
    }
    memset(out, 0, prefix_len + password_len);
    write_le32(out, timestamp);
    if (role == D1L_MESHCORE_ADMIN_ROLE_ROOM) {
        write_le32(&out[4], room_sync_since);
    }
    if (password_len > 0U) {
        memcpy(&out[prefix_len], password, password_len);
    }
    *out_len = prefix_len + password_len;
    return true;
}

bool d1l_meshcore_admin_encode_status_request(
    uint32_t tag, uint32_t uniqueness,
    uint8_t out[D1L_MESHCORE_ADMIN_REQUEST_BYTES])
{
    if (!out || tag == 0U) {
        return false;
    }
    memset(out, 0, D1L_MESHCORE_ADMIN_REQUEST_BYTES);
    write_le32(out, tag);
    out[4] = D1L_MESHCORE_ADMIN_REQUEST_GET_STATUS;
    write_le32(&out[9], uniqueness);
    return true;
}

const char *d1l_meshcore_admin_query_name(
    d1l_meshcore_admin_query_t query)
{
    switch (query) {
    case D1L_MESHCORE_ADMIN_QUERY_TELEMETRY:
        return "telemetry";
    case D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST:
        return "access_list";
    case D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS:
        return "neighbours";
    case D1L_MESHCORE_ADMIN_QUERY_NONE:
    default:
        return "none";
    }
}

bool d1l_meshcore_admin_query_allowed(
    d1l_meshcore_admin_role_t role, uint8_t permissions,
    d1l_meshcore_admin_query_t query)
{
    if (role != D1L_MESHCORE_ADMIN_ROLE_REPEATER &&
        role != D1L_MESHCORE_ADMIN_ROLE_ROOM) {
        return false;
    }
    const uint8_t permission_role =
        permissions & D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK;
    switch (query) {
    case D1L_MESHCORE_ADMIN_QUERY_TELEMETRY:
        return true;
    case D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST:
        return permission_role == D1L_MESHCORE_ADMIN_PERMISSION_ADMIN;
    case D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS:
        return role == D1L_MESHCORE_ADMIN_ROLE_REPEATER;
    case D1L_MESHCORE_ADMIN_QUERY_NONE:
    default:
        return false;
    }
}

bool d1l_meshcore_admin_encode_query_request(
    d1l_meshcore_admin_query_t query, uint32_t tag, uint16_t offset,
    uint32_t uniqueness, uint8_t *out, size_t out_size, size_t *out_len)
{
    if (!out || !out_len || tag == 0U ||
        (query != D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS && offset != 0U)) {
        return false;
    }
    size_t logical_len = 0U;
    switch (query) {
    case D1L_MESHCORE_ADMIN_QUERY_TELEMETRY:
        logical_len = D1L_MESHCORE_ADMIN_REQUEST_BYTES;
        break;
    case D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST:
        logical_len = 11U;
        break;
    case D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS:
        logical_len = D1L_MESHCORE_ADMIN_MAX_QUERY_REQUEST_BYTES;
        break;
    case D1L_MESHCORE_ADMIN_QUERY_NONE:
    default:
        return false;
    }
    if (out_size < logical_len) {
        return false;
    }
    memset(out, 0, logical_len);
    write_le32(out, tag);
    if (query == D1L_MESHCORE_ADMIN_QUERY_TELEMETRY) {
        out[4] = D1L_MESHCORE_ADMIN_REQUEST_GET_TELEMETRY;
        /* An inverse zero mask requests every field permitted by the
         * authenticated session. A guest server still limits this to base
         * telemetry. */
        out[5] = 0U;
        write_le32(&out[9], uniqueness);
    } else if (query == D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST) {
        out[4] = D1L_MESHCORE_ADMIN_REQUEST_GET_ACCESS_LIST;
        write_le32(&out[7], uniqueness);
    } else {
        out[4] = D1L_MESHCORE_ADMIN_REQUEST_GET_NEIGHBOURS;
        out[5] = 0U;
        out[6] = D1L_MESHCORE_ADMIN_NEIGHBOUR_PAGE_COUNT;
        write_le16(&out[7], offset);
        out[9] = 0U;
        out[10] = D1L_MESHCORE_ADMIN_NEIGHBOUR_PREFIX_BYTES;
        write_le32(&out[11], uniqueness);
    }
    *out_len = logical_len;
    return true;
}

static uint32_t next_generation(uint32_t current)
{
    return current == UINT32_MAX ? 1U : current + 1U;
}

void d1l_meshcore_admin_reset(d1l_meshcore_admin_session_t *session)
{
    if (!session) {
        return;
    }
    const uint32_t generation = next_generation(session->generation);
    d1l_meshcore_admin_secure_zero(session, sizeof(*session));
    session->state = D1L_MESHCORE_ADMIN_IDLE;
    session->generation = generation;
}

void d1l_meshcore_admin_timeout(d1l_meshcore_admin_session_t *session)
{
    (void)d1l_meshcore_admin_fail(
        session, D1L_MESHCORE_ADMIN_TIMED_OUT);
}

bool d1l_meshcore_admin_fail(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_state_t failure_state)
{
    if (!session ||
        (failure_state != D1L_MESHCORE_ADMIN_TIMED_OUT &&
         failure_state != D1L_MESHCORE_ADMIN_REJECTED_CREDENTIALS &&
         failure_state != D1L_MESHCORE_ADMIN_DISCONNECTED &&
         failure_state != D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL &&
         failure_state != D1L_MESHCORE_ADMIN_RADIO_BUSY &&
         failure_state != D1L_MESHCORE_ADMIN_VOLATILE_REPLAY_REJECTED &&
         failure_state != D1L_MESHCORE_ADMIN_DURABLE_REPLAY_REJECTED &&
         failure_state != D1L_MESHCORE_ADMIN_LOCAL_STORAGE_FAILED)) {
        return false;
    }
    const uint32_t generation = next_generation(session->generation);
    const d1l_meshcore_admin_role_t role = session->role;
    d1l_meshcore_admin_secure_zero(session, sizeof(*session));
    session->state = failure_state;
    session->role = role;
    session->generation = generation;
    return true;
}

void d1l_meshcore_admin_replay_cache_clear(
    d1l_meshcore_admin_replay_cache_t *cache)
{
    d1l_meshcore_admin_secure_zero(cache, cache ? sizeof(*cache) : 0U);
}

bool d1l_meshcore_admin_begin_login(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_role_t role,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t local_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t session_secret[D1L_MESHCORE_ADMIN_SECRET_BYTES],
    uint64_t request_deadline_us, uint64_t idle_timeout_us,
    uint64_t absolute_timeout_us)
{
    if (!session || !peer_public_key || !local_public_key || !session_secret ||
        request_deadline_us == 0U || idle_timeout_us == 0U ||
        absolute_timeout_us == 0U || idle_timeout_us > absolute_timeout_us ||
        (role != D1L_MESHCORE_ADMIN_ROLE_REPEATER &&
         role != D1L_MESHCORE_ADMIN_ROLE_ROOM) ||
        (session->state != D1L_MESHCORE_ADMIN_IDLE &&
         session->state != D1L_MESHCORE_ADMIN_TIMED_OUT &&
         session->state != D1L_MESHCORE_ADMIN_REJECTED_CREDENTIALS &&
         session->state != D1L_MESHCORE_ADMIN_DISCONNECTED &&
         session->state != D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL &&
         session->state != D1L_MESHCORE_ADMIN_RADIO_BUSY &&
         session->state != D1L_MESHCORE_ADMIN_VOLATILE_REPLAY_REJECTED &&
         session->state != D1L_MESHCORE_ADMIN_DURABLE_REPLAY_REJECTED &&
         session->state != D1L_MESHCORE_ADMIN_LOCAL_STORAGE_FAILED)) {
        return false;
    }
    const uint32_t generation = next_generation(session->generation);
    d1l_meshcore_admin_secure_zero(session, sizeof(*session));
    session->state = D1L_MESHCORE_ADMIN_LOGIN_PENDING;
    session->role = role;
    session->generation = generation;
    memcpy(session->peer_public_key, peer_public_key,
           sizeof(session->peer_public_key));
    memcpy(session->local_public_key, local_public_key,
           sizeof(session->local_public_key));
    memcpy(session->session_secret, session_secret,
           sizeof(session->session_secret));
    session->request_deadline_us = request_deadline_us;
    session->idle_timeout_us = idle_timeout_us;
    session->absolute_timeout_us = absolute_timeout_us;
    return true;
}

bool d1l_meshcore_admin_peer_matches(
    const d1l_meshcore_admin_session_t *session,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES])
{
    return session && peer_public_key &&
           memcmp(session->peer_public_key, peer_public_key,
                  D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES) == 0;
}

bool d1l_meshcore_admin_binding_matches(
    const d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_role_t role,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t local_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t session_secret[D1L_MESHCORE_ADMIN_SECRET_BYTES])
{
    return session && role == session->role &&
           (role == D1L_MESHCORE_ADMIN_ROLE_REPEATER ||
            role == D1L_MESHCORE_ADMIN_ROLE_ROOM) &&
           bytes_equal(session->peer_public_key, peer_public_key,
                       D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES) &&
           bytes_equal(session->local_public_key, local_public_key,
                       D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES) &&
           bytes_equal(session->session_secret, session_secret,
                       D1L_MESHCORE_ADMIN_SECRET_BYTES);
}

static bool replay_cache_contains(
    const d1l_meshcore_admin_replay_cache_t *cache,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t response[D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES],
    uint32_t server_timestamp)
{
    if (!cache) {
        return false;
    }
    for (size_t i = 0U; i < D1L_MESHCORE_ADMIN_REPLAY_PEER_CAPACITY; ++i) {
        const d1l_meshcore_admin_replay_entry_t *entry = &cache->peers[i];
        if (!entry->valid ||
            !bytes_equal(entry->peer_public_key, peer_public_key,
                         D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES)) {
            continue;
        }
        if (server_timestamp <= entry->highest_server_timestamp) {
            return true;
        }
        for (size_t response_index = 0U;
             response_index < entry->response_count; ++response_index) {
            if (bytes_equal(
                    entry->responses[response_index], response,
                    D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES)) {
                return true;
            }
        }
    }
    return false;
}

static void replay_cache_remember(
    d1l_meshcore_admin_replay_cache_t *cache,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t response[D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES],
    uint32_t server_timestamp)
{
    size_t slot = D1L_MESHCORE_ADMIN_REPLAY_PEER_CAPACITY;
    for (size_t i = 0U; i < D1L_MESHCORE_ADMIN_REPLAY_PEER_CAPACITY; ++i) {
        if (cache->peers[i].valid &&
            bytes_equal(cache->peers[i].peer_public_key, peer_public_key,
                        D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES)) {
            slot = i;
            break;
        }
        if (slot == D1L_MESHCORE_ADMIN_REPLAY_PEER_CAPACITY &&
            !cache->peers[i].valid) {
            slot = i;
        }
    }
    if (slot == D1L_MESHCORE_ADMIN_REPLAY_PEER_CAPACITY) {
        slot = cache->next_replacement;
        cache->next_replacement = (uint8_t)(
            (cache->next_replacement + 1U) %
            D1L_MESHCORE_ADMIN_REPLAY_PEER_CAPACITY);
    }
    d1l_meshcore_admin_replay_entry_t *entry = &cache->peers[slot];
    if (!entry->valid ||
        !bytes_equal(entry->peer_public_key, peer_public_key,
                     D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES)) {
        d1l_meshcore_admin_secure_zero(entry, sizeof(*entry));
        memcpy(entry->peer_public_key, peer_public_key,
               sizeof(entry->peer_public_key));
        entry->valid = true;
    }
    size_t response_slot;
    if (entry->response_count <
        D1L_MESHCORE_ADMIN_REPLAY_RESPONSES_PER_PEER) {
        response_slot = entry->response_count++;
        if (entry->response_count ==
            D1L_MESHCORE_ADMIN_REPLAY_RESPONSES_PER_PEER) {
            entry->next_response = 0U;
        }
    } else {
        response_slot = entry->next_response;
        entry->next_response = (uint8_t)(
            (entry->next_response + 1U) %
            D1L_MESHCORE_ADMIN_REPLAY_RESPONSES_PER_PEER);
    }
    memcpy(entry->responses[response_slot], response,
           D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES);
    entry->highest_server_timestamp = server_timestamp;
}

static bool authenticated_deadline_due(
    const d1l_meshcore_admin_session_t *session, uint64_t now_us)
{
    return deadline_due(session->idle_deadline_us, now_us) ||
           deadline_due(session->absolute_deadline_us, now_us);
}

static void refresh_idle_deadline(d1l_meshcore_admin_session_t *session,
                                  uint64_t now_us)
{
    uint64_t refreshed = deadline_after(now_us, session->idle_timeout_us);
    if (refreshed > session->absolute_deadline_us) {
        refreshed = session->absolute_deadline_us;
    }
    session->idle_deadline_us = refreshed;
}

bool d1l_meshcore_admin_note_authenticated_activity(
    d1l_meshcore_admin_session_t *session, uint64_t now_us)
{
    if (!session ||
        (session->state != D1L_MESHCORE_ADMIN_AUTHENTICATED &&
         session->state != D1L_MESHCORE_ADMIN_STATUS_PENDING &&
         session->state != D1L_MESHCORE_ADMIN_MUTATION_PENDING &&
         session->state != D1L_MESHCORE_ADMIN_CLI_PENDING &&
         session->state != D1L_MESHCORE_ADMIN_QUERY_PENDING)) {
        return false;
    }
    if (authenticated_deadline_due(session, now_us)) {
        d1l_meshcore_admin_timeout(session);
        return false;
    }
    refresh_idle_deadline(session, now_us);
    return true;
}

bool d1l_meshcore_admin_canonical_span(const uint8_t *data, size_t data_len,
                                       size_t logical_len)
{
    if (!data || data_len < logical_len ||
        data_len > logical_len + D1L_MESHCORE_ADMIN_MAX_PADDING_BYTES) {
        return false;
    }
    for (size_t i = logical_len; i < data_len; ++i) {
        if (data[i] != 0U) {
            return false;
        }
    }
    return true;
}

d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_accept_login_response(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_replay_cache_t *replay_cache,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t *plaintext, size_t plaintext_len, uint64_t now_us)
{
    if (!session || !replay_cache ||
        session->state != D1L_MESHCORE_ADMIN_LOGIN_PENDING ||
        !d1l_meshcore_admin_peer_matches(session, peer_public_key)) {
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    if (deadline_due(session->request_deadline_us, now_us)) {
        d1l_meshcore_admin_timeout(session);
        return D1L_MESHCORE_ADMIN_RESPONSE_EXPIRED;
    }
    if (!d1l_meshcore_admin_canonical_span(
            plaintext, plaintext_len,
            D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES)) {
        (void)d1l_meshcore_admin_fail(
            session, D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL);
        return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
    }

    if (plaintext[4] != 0U) {
        (void)d1l_meshcore_admin_fail(
            session, D1L_MESHCORE_ADMIN_REJECTED_CREDENTIALS);
        return D1L_MESHCORE_ADMIN_RESPONSE_REJECTED;
    }
    const uint8_t permissions = plaintext[7];
    const uint8_t permission_role =
        permissions & D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK;
    const uint8_t expected_firmware =
        session->role == D1L_MESHCORE_ADMIN_ROLE_REPEATER ? 2U : 1U;
    const uint8_t expected_legacy_role =
        permission_role == D1L_MESHCORE_ADMIN_PERMISSION_ADMIN ? 1U :
        (session->role == D1L_MESHCORE_ADMIN_ROLE_ROOM &&
         permission_role == D1L_MESHCORE_ADMIN_PERMISSION_GUEST ? 2U : 0U);
    if (plaintext[5] != 0U || plaintext[6] != expected_legacy_role ||
        plaintext[12] != expected_firmware) {
        (void)d1l_meshcore_admin_fail(
            session, D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL);
        return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
    }
    const uint32_t server_timestamp = read_le32(plaintext);
    if (server_timestamp == 0U) {
        (void)d1l_meshcore_admin_fail(
            session, D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL);
        return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
    }
    if (replay_cache_contains(
            replay_cache, peer_public_key, plaintext, server_timestamp)) {
        (void)d1l_meshcore_admin_fail(
            session, D1L_MESHCORE_ADMIN_VOLATILE_REPLAY_REJECTED);
        return D1L_MESHCORE_ADMIN_RESPONSE_REPLAYED;
    }

    replay_cache_remember(
        replay_cache, peer_public_key, plaintext, server_timestamp);
    session->server_timestamp = server_timestamp;
    session->permissions = permissions;
    session->firmware_level = plaintext[12];
    session->request_deadline_us = 0U;
    session->absolute_deadline_us =
        deadline_after(now_us, session->absolute_timeout_us);
    refresh_idle_deadline(session, now_us);
    session->state = D1L_MESHCORE_ADMIN_AUTHENTICATED;
    session->generation = next_generation(session->generation);
    return D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED;
}

bool d1l_meshcore_admin_begin_status_request(
    d1l_meshcore_admin_session_t *session, uint32_t tag,
    uint64_t now_us, uint64_t request_deadline_us)
{
    if (!session || session->state != D1L_MESHCORE_ADMIN_AUTHENTICATED ||
        tag == 0U || request_deadline_us == 0U ||
        tag == session->last_completed_tag) {
        return false;
    }
    if (authenticated_deadline_due(session, now_us)) {
        d1l_meshcore_admin_timeout(session);
        return false;
    }
    session->pending_tag = tag;
    session->request_deadline_us = request_deadline_us;
    session->state = D1L_MESHCORE_ADMIN_STATUS_PENDING;
    session->generation = next_generation(session->generation);
    return true;
}

bool d1l_meshcore_admin_cancel_status_request(
    d1l_meshcore_admin_session_t *session, uint32_t tag)
{
    if (!session || session->state != D1L_MESHCORE_ADMIN_STATUS_PENDING ||
        session->pending_tag != tag) {
        return false;
    }
    session->pending_tag = 0U;
    session->request_deadline_us = 0U;
    session->state = D1L_MESHCORE_ADMIN_AUTHENTICATED;
    session->generation = next_generation(session->generation);
    return true;
}

bool d1l_meshcore_admin_begin_query_request(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_query_t query, uint16_t offset, uint32_t tag,
    uint64_t now_us, uint64_t request_deadline_us)
{
    if (!session || session->state != D1L_MESHCORE_ADMIN_AUTHENTICATED ||
        !d1l_meshcore_admin_query_allowed(
            session->role, session->permissions, query) ||
        (query != D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS && offset != 0U) ||
        tag == 0U || request_deadline_us == 0U ||
        tag == session->last_completed_tag) {
        return false;
    }
    if (authenticated_deadline_due(session, now_us)) {
        d1l_meshcore_admin_timeout(session);
        return false;
    }
    session->pending_tag = tag;
    session->pending_query = query;
    session->pending_query_offset = offset;
    session->request_deadline_us = request_deadline_us;
    session->state = D1L_MESHCORE_ADMIN_QUERY_PENDING;
    session->generation = next_generation(session->generation);
    return true;
}

bool d1l_meshcore_admin_cancel_query_request(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_query_t query, uint32_t tag)
{
    if (!session || session->state != D1L_MESHCORE_ADMIN_QUERY_PENDING ||
        session->pending_query != query || session->pending_tag != tag) {
        return false;
    }
    session->pending_tag = 0U;
    session->pending_query = D1L_MESHCORE_ADMIN_QUERY_NONE;
    session->pending_query_offset = 0U;
    session->request_deadline_us = 0U;
    session->state = D1L_MESHCORE_ADMIN_AUTHENTICATED;
    session->generation = next_generation(session->generation);
    return true;
}

const char *d1l_meshcore_admin_mutation_name(
    d1l_meshcore_admin_mutation_t mutation)
{
    switch (mutation) {
    case D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS:
        return "clear_stats";
    case D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP:
        return "advertise_zero_hop";
    case D1L_MESHCORE_ADMIN_MUTATION_NONE:
    default:
        return "none";
    }
}

const char *d1l_meshcore_admin_mutation_command(
    d1l_meshcore_admin_mutation_t mutation)
{
    switch (mutation) {
    case D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS:
        return "clear stats";
    case D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP:
        return "advert.zerohop";
    case D1L_MESHCORE_ADMIN_MUTATION_NONE:
    default:
        return NULL;
    }
}

static const char *mutation_success_reply(
    d1l_meshcore_admin_mutation_t mutation)
{
    switch (mutation) {
    case D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS:
        return "(OK - stats reset)";
    case D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP:
        return "OK - zerohop advert sent";
    case D1L_MESHCORE_ADMIN_MUTATION_NONE:
    default:
        return NULL;
    }
}

bool d1l_meshcore_admin_begin_mutation(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_mutation_t mutation, uint32_t tag,
    uint64_t now_us, uint64_t request_deadline_us)
{
    const uint8_t expected_firmware =
        session && session->role == D1L_MESHCORE_ADMIN_ROLE_REPEATER ? 2U :
        session && session->role == D1L_MESHCORE_ADMIN_ROLE_ROOM ? 1U : 0U;
    if (!session || session->state != D1L_MESHCORE_ADMIN_AUTHENTICATED ||
        !d1l_meshcore_admin_mutation_command(mutation) ||
        tag == 0U || request_deadline_us == 0U ||
        tag == session->last_completed_tag ||
        (session->permissions & D1L_MESHCORE_ADMIN_PERMISSION_ADMIN) !=
            D1L_MESHCORE_ADMIN_PERMISSION_ADMIN ||
        expected_firmware == 0U ||
        session->firmware_level != expected_firmware) {
        return false;
    }
    if (authenticated_deadline_due(session, now_us)) {
        d1l_meshcore_admin_timeout(session);
        return false;
    }
    session->pending_tag = tag;
    session->pending_mutation = mutation;
    session->request_deadline_us = request_deadline_us;
    session->state = D1L_MESHCORE_ADMIN_MUTATION_PENDING;
    session->generation = next_generation(session->generation);
    return true;
}

bool d1l_meshcore_admin_cancel_mutation(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_mutation_t mutation, uint32_t tag)
{
    if (!session ||
        session->state != D1L_MESHCORE_ADMIN_MUTATION_PENDING ||
        session->pending_mutation != mutation ||
        session->pending_tag != tag) {
        return false;
    }
    session->pending_tag = 0U;
    session->pending_mutation = D1L_MESHCORE_ADMIN_MUTATION_NONE;
    session->request_deadline_us = 0U;
    session->state = D1L_MESHCORE_ADMIN_AUTHENTICATED;
    session->generation = next_generation(session->generation);
    return true;
}

static uint8_t cli_ascii_lower(uint8_t value)
{
    return value >= (uint8_t)'A' && value <= (uint8_t)'Z' ?
        (uint8_t)(value + ((uint8_t)'a' - (uint8_t)'A')) : value;
}

enum {
    CLI_ROLE_REPEATER = 0x01U,
    CLI_ROLE_ROOM = 0x02U,
    CLI_ROLE_BOTH = CLI_ROLE_REPEATER | CLI_ROLE_ROOM,
};

typedef struct {
    d1l_meshcore_admin_cli_policy_t policy;
    uint8_t role_mask;
} cli_classification_t;

typedef struct {
    const char *name;
    uint8_t role_mask;
    bool readable;
    bool writable;
    bool sensitive;
} cli_setting_t;

typedef struct {
    const char *command;
    d1l_meshcore_admin_cli_policy_t policy;
    uint8_t role_mask;
} cli_exact_rule_t;

static const cli_setting_t CLI_SETTINGS[] = {
    {"dutycycle", CLI_ROLE_BOTH, true, true, false},
    {"af", CLI_ROLE_BOTH, true, true, false},
    {"int.thresh", CLI_ROLE_BOTH, true, true, false},
    {"agc.reset.interval", CLI_ROLE_BOTH, true, true, false},
    {"multi.acks", CLI_ROLE_BOTH, true, true, false},
    {"allow.read.only", CLI_ROLE_ROOM, true, true, false},
    {"flood.advert.interval", CLI_ROLE_BOTH, true, true, false},
    {"advert.interval", CLI_ROLE_BOTH, true, true, false},
    {"guest.password", CLI_ROLE_BOTH, true, true, true},
    {"name", CLI_ROLE_BOTH, true, true, false},
    {"repeat", CLI_ROLE_BOTH, true, true, false},
    {"radio.rxgain", CLI_ROLE_BOTH, true, true, false},
    {"radio", CLI_ROLE_BOTH, true, true, false},
    {"lat", CLI_ROLE_BOTH, true, true, false},
    {"lon", CLI_ROLE_BOTH, true, true, false},
    {"rxdelay", CLI_ROLE_BOTH, true, true, false},
    {"txdelay", CLI_ROLE_BOTH, true, true, false},
    {"flood.max.unscoped", CLI_ROLE_BOTH, true, true, false},
    {"flood.max.advert", CLI_ROLE_BOTH, true, true, false},
    {"flood.max", CLI_ROLE_BOTH, true, true, false},
    {"direct.txdelay", CLI_ROLE_BOTH, true, true, false},
    {"owner.info", CLI_ROLE_BOTH, true, true, false},
    {"path.hash.mode", CLI_ROLE_BOTH, true, true, false},
    {"loop.detect", CLI_ROLE_BOTH, true, true, false},
    {"tx", CLI_ROLE_BOTH, true, true, false},
    {"bridge.enabled", CLI_ROLE_BOTH, true, true, false},
    {"bridge.delay", CLI_ROLE_BOTH, true, true, false},
    {"bridge.source", CLI_ROLE_BOTH, true, true, false},
    {"bridge.baud", CLI_ROLE_BOTH, true, true, false},
    {"bridge.channel", CLI_ROLE_BOTH, true, true, false},
    {"bridge.secret", CLI_ROLE_BOTH, true, true, true},
    {"adc.multiplier", CLI_ROLE_BOTH, true, true, false},
    {"prv.key", CLI_ROLE_BOTH, false, true, true},
    {"freq", CLI_ROLE_BOTH, true, false, false},
    {"public.key", CLI_ROLE_BOTH, true, false, false},
    {"role", CLI_ROLE_BOTH, true, false, false},
    {"bridge.type", CLI_ROLE_BOTH, true, false, false},
    {"bootloader.ver", CLI_ROLE_BOTH, true, false, false},
    {"pwrmgt.support", CLI_ROLE_BOTH, true, false, false},
    {"pwrmgt.source", CLI_ROLE_BOTH, true, false, false},
    {"pwrmgt.bootreason", CLI_ROLE_BOTH, true, false, false},
    {"pwrmgt.bootmv", CLI_ROLE_BOTH, true, false, false},
};

static const cli_exact_rule_t CLI_EXACT_RULES[] = {
    {"ver", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH},
    {"board", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH},
    {"clock", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH},
    {"neighbors", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_REPEATER},
    {"powersaving", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_REPEATER},
    {"gps", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH},
    {"gps advert", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH},
    {"region", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH},
    {"region home", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH},
    {"region default", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH},
    {"region list allowed", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH},
    {"region list denied", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH},
    {"sensor list", D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH},
    {"clock sync", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"advert", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"advert.zerohop", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"clear stats", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"log start", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"log stop", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"log erase", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"gps on", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"gps off", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"gps sync", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"gps advert none", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"gps advert share", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"gps advert prefs", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"region save", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH},
    {"discover.neighbors", D1L_MESHCORE_ADMIN_CLI_MUTATION,
     CLI_ROLE_REPEATER},
    {"powersaving on", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_REPEATER},
    {"powersaving off", D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_REPEATER},
};

static bool cli_command_shape_valid(const char *command)
{
    if (!command) {
        return false;
    }
    size_t command_len = 0U;
    while (command_len <= D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES &&
           command[command_len] != '\0') {
        command_len++;
    }
    if (command_len == 0U ||
        command_len > D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES ||
        command[0] == ' ' || command[command_len - 1U] == ' ') {
        return false;
    }
    for (size_t i = 0U; i < command_len; ++i) {
        const uint8_t value = (uint8_t)command[i];
        if (value < 0x20U || value > 0x7EU) {
            return false;
        }
    }
    return true;
}

static bool cli_starts_with(const char *command, const char *prefix)
{
    return command && prefix &&
           strncmp(command, prefix, strlen(prefix)) == 0;
}

static bool cli_single_token(const char *value)
{
    return value && value[0] != '\0' && strchr(value, ' ') == NULL;
}

static bool cli_unsigned_decimal(const char *value)
{
    if (!cli_single_token(value)) {
        return false;
    }
    for (size_t i = 0U; value[i] != '\0'; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool cli_hex_span(const char *value, size_t len)
{
    if (!value || len == 0U) {
        return false;
    }
    for (size_t i = 0U; i < len; ++i) {
        const char ch = value[i];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool cli_hex_token(const char *value, size_t min_len, size_t max_len)
{
    if (!cli_single_token(value)) {
        return false;
    }
    const size_t len = strlen(value);
    return len >= min_len && len <= max_len && (len & 1U) == 0U &&
           cli_hex_span(value, len);
}

static const cli_setting_t *cli_find_setting(
    const char *name, size_t name_len)
{
    for (size_t i = 0U;
         i < sizeof(CLI_SETTINGS) / sizeof(CLI_SETTINGS[0]); ++i) {
        if (strlen(CLI_SETTINGS[i].name) == name_len &&
            strncmp(name, CLI_SETTINGS[i].name, name_len) == 0) {
            return &CLI_SETTINGS[i];
        }
    }
    return NULL;
}

static cli_classification_t cli_classification(
    d1l_meshcore_admin_cli_policy_t policy, uint8_t role_mask)
{
    return (cli_classification_t) {
        .policy = policy,
        .role_mask = role_mask,
    };
}

static cli_classification_t cli_classify_setting(const char *command)
{
    const bool is_get = cli_starts_with(command, "get ");
    const bool is_set = cli_starts_with(command, "set ");
    if (!is_get && !is_set) {
        return cli_classification(
            D1L_MESHCORE_ADMIN_CLI_UNSUPPORTED, 0U);
    }
    const char *name = command + 4U;
    const char *value = strchr(name, ' ');
    const size_t name_len = value ? (size_t)(value - name) : strlen(name);
    const cli_setting_t *setting = cli_find_setting(name, name_len);
    if (!setting || name_len == 0U) {
        return cli_classification(
            D1L_MESHCORE_ADMIN_CLI_UNSUPPORTED, 0U);
    }
    if (is_get) {
        if (value || !setting->readable) {
            return cli_classification(
                D1L_MESHCORE_ADMIN_CLI_UNSUPPORTED, 0U);
        }
        return cli_classification(
            setting->sensitive ? D1L_MESHCORE_ADMIN_CLI_SENSITIVE :
                                 D1L_MESHCORE_ADMIN_CLI_READ_ONLY,
            setting->role_mask);
    }
    if (!value || value[1] == '\0' || !setting->writable) {
        return cli_classification(
            D1L_MESHCORE_ADMIN_CLI_UNSUPPORTED, 0U);
    }
    return cli_classification(
        setting->sensitive ? D1L_MESHCORE_ADMIN_CLI_SENSITIVE :
                             D1L_MESHCORE_ADMIN_CLI_MUTATION,
        setting->role_mask);
}

static bool cli_acl_arguments_valid(const char *arguments)
{
    if (!arguments) {
        return false;
    }
    const char *permission = strchr(arguments, ' ');
    return permission &&
           permission == arguments + (D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES * 2U) &&
           cli_hex_span(
               arguments, D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES * 2U) &&
           permission[1] >= '0' && permission[1] <= '3' &&
           permission[2] == '\0';
}

static cli_classification_t cli_classify(const char *command)
{
    if (!cli_command_shape_valid(command)) {
        return cli_classification(
            D1L_MESHCORE_ADMIN_CLI_UNSUPPORTED, 0U);
    }

    const cli_classification_t setting = cli_classify_setting(command);
    if (setting.policy != D1L_MESHCORE_ADMIN_CLI_UNSUPPORTED) {
        return setting;
    }
    for (size_t i = 0U;
         i < sizeof(CLI_EXACT_RULES) / sizeof(CLI_EXACT_RULES[0]); ++i) {
        if (strcmp(command, CLI_EXACT_RULES[i].command) == 0) {
            return cli_classification(
                CLI_EXACT_RULES[i].policy, CLI_EXACT_RULES[i].role_mask);
        }
    }

    if (cli_starts_with(command, "setperm ") &&
        cli_acl_arguments_valid(command + strlen("setperm "))) {
        return cli_classification(
            D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH);
    }
    if (cli_starts_with(command, "region get ") &&
        cli_single_token(command + strlen("region get "))) {
        return cli_classification(
            D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH);
    }
    if (cli_starts_with(command, "sensor list ") &&
        cli_unsigned_decimal(command + strlen("sensor list "))) {
        return cli_classification(
            D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH);
    }
    if (cli_starts_with(command, "sensor get ") &&
        cli_single_token(command + strlen("sensor get "))) {
        return cli_classification(
            D1L_MESHCORE_ADMIN_CLI_READ_ONLY, CLI_ROLE_BOTH);
    }
    if (cli_starts_with(command, "time ") &&
        cli_unsigned_decimal(command + strlen("time "))) {
        return cli_classification(
            D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH);
    }
    if (cli_starts_with(command, "neighbor.remove ") &&
        cli_hex_token(
            command + strlen("neighbor.remove "), 2U,
            D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES * 2U)) {
        return cli_classification(
            D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_REPEATER);
    }
    if (cli_starts_with(command, "password ") &&
        command[strlen("password ")] != '\0') {
        return cli_classification(
            D1L_MESHCORE_ADMIN_CLI_SENSITIVE, CLI_ROLE_BOTH);
    }
    if (cli_starts_with(command, "tempradio ") ||
        cli_starts_with(command, "gps setloc ")) {
        return cli_classification(
            D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH);
    }
    if (cli_starts_with(command, "sensor set ")) {
        const char *arguments = command + strlen("sensor set ");
        const char *value = strchr(arguments, ' ');
        if (value && value != arguments && value[1] != '\0') {
            return cli_classification(
                D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH);
        }
    }
    static const char *const REGION_MUTATION_PREFIXES[] = {
        "region allowf ", "region denyf ", "region put ", "region def ",
        "region remove ", "region home ", "region default ",
    };
    for (size_t i = 0U;
         i < sizeof(REGION_MUTATION_PREFIXES) /
                 sizeof(REGION_MUTATION_PREFIXES[0]); ++i) {
        if (cli_starts_with(command, REGION_MUTATION_PREFIXES[i]) &&
            command[strlen(REGION_MUTATION_PREFIXES[i])] != '\0') {
            return cli_classification(
                D1L_MESHCORE_ADMIN_CLI_MUTATION, CLI_ROLE_BOTH);
        }
    }
    return cli_classification(
        D1L_MESHCORE_ADMIN_CLI_UNSUPPORTED, 0U);
}

d1l_meshcore_admin_cli_policy_t d1l_meshcore_admin_cli_command_policy(
    const char *command)
{
    return cli_classify(command).policy;
}

bool d1l_meshcore_admin_cli_command_valid(const char *command)
{
    return d1l_meshcore_admin_cli_command_policy(command) !=
           D1L_MESHCORE_ADMIN_CLI_UNSUPPORTED;
}

bool d1l_meshcore_admin_cli_command_sensitive(const char *command)
{
    return d1l_meshcore_admin_cli_command_policy(command) ==
           D1L_MESHCORE_ADMIN_CLI_SENSITIVE;
}

bool d1l_meshcore_admin_cli_command_read_only(const char *command)
{
    return d1l_meshcore_admin_cli_command_policy(command) ==
           D1L_MESHCORE_ADMIN_CLI_READ_ONLY;
}

d1l_meshcore_admin_cli_reply_profile_t
d1l_meshcore_admin_cli_command_reply_profile(const char *command)
{
    if (command && strcmp(command, "get bootloader.ver") == 0) {
        return D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNKNOWN_VALUE;
    }
    if (command && strcmp(command, "get pwrmgt.support") == 0) {
        return D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNSUPPORTED_VALUE;
    }
    if (command && strcmp(command, "get adc.multiplier") == 0) {
        return D1L_MESHCORE_ADMIN_CLI_REPLY_ADC_UNSUPPORTED;
    }
    if (command &&
        (strcmp(command, "get pwrmgt.source") == 0 ||
         strcmp(command, "get pwrmgt.bootreason") == 0 ||
         strcmp(command, "get pwrmgt.bootmv") == 0)) {
        return D1L_MESHCORE_ADMIN_CLI_REPLY_POWER_MANAGEMENT_UNSUPPORTED;
    }
    if (command && strcmp(command, "gps") == 0) {
        return D1L_MESHCORE_ADMIN_CLI_REPLY_GPS_NOT_FOUND;
    }
    if (command && strcmp(command, "gps advert") == 0) {
        return D1L_MESHCORE_ADMIN_CLI_REPLY_GPS_ADVERT_ERROR;
    }
    if (command && strncmp(command, "region get ", 11U) == 0) {
        return D1L_MESHCORE_ADMIN_CLI_REPLY_REGION_NOT_FOUND;
    }
    if (command && strncmp(command, "get ", 4U) == 0) {
        return D1L_MESHCORE_ADMIN_CLI_REPLY_GET_VALUE;
    }
    return D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT;
}

bool d1l_meshcore_admin_cli_command_allowed(
    const char *command, d1l_meshcore_admin_role_t role,
    uint8_t permissions)
{
    const cli_classification_t classification = cli_classify(command);
    const uint8_t role_mask =
        role == D1L_MESHCORE_ADMIN_ROLE_REPEATER ? CLI_ROLE_REPEATER :
        role == D1L_MESHCORE_ADMIN_ROLE_ROOM ? CLI_ROLE_ROOM : 0U;
    return classification.policy != D1L_MESHCORE_ADMIN_CLI_UNSUPPORTED &&
           role_mask != 0U &&
           (classification.role_mask & role_mask) != 0U &&
           (permissions & D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK) ==
               D1L_MESHCORE_ADMIN_PERMISSION_ADMIN;
}

bool d1l_meshcore_admin_format_acl_command(
    const char *full_public_key_hex, uint8_t permissions,
    char *out_command, size_t out_command_size)
{
    if (!full_public_key_hex || !out_command ||
        permissions > D1L_MESHCORE_ADMIN_PERMISSION_ADMIN ||
        !cli_hex_token(
            full_public_key_hex,
            D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES * 2U,
            D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES * 2U)) {
        return false;
    }
    const int written = snprintf(
        out_command, out_command_size, "setperm %s %u",
        full_public_key_hex, (unsigned)permissions);
    if (written < 0 || (size_t)written >= out_command_size) {
        if (out_command_size > 0U) {
            out_command[0] = '\0';
        }
        return false;
    }
    return true;
}

bool d1l_meshcore_admin_begin_cli_command(
    d1l_meshcore_admin_session_t *session, uint32_t tag,
    bool sensitive, bool read_only,
    d1l_meshcore_admin_cli_reply_profile_t reply_profile, uint64_t now_us,
    uint64_t request_deadline_us)
{
    const uint8_t expected_firmware =
        session && session->role == D1L_MESHCORE_ADMIN_ROLE_REPEATER ? 2U :
        session && session->role == D1L_MESHCORE_ADMIN_ROLE_ROOM ? 1U : 0U;
    if (!session || session->state != D1L_MESHCORE_ADMIN_AUTHENTICATED ||
        tag == 0U || request_deadline_us == 0U ||
        tag == session->last_completed_tag ||
        (session->permissions & D1L_MESHCORE_ADMIN_PERMISSION_ADMIN) !=
            D1L_MESHCORE_ADMIN_PERMISSION_ADMIN ||
        expected_firmware == 0U ||
        session->firmware_level != expected_firmware ||
        reply_profile > D1L_MESHCORE_ADMIN_CLI_REPLY_REGION_NOT_FOUND) {
        return false;
    }
    if (authenticated_deadline_due(session, now_us)) {
        d1l_meshcore_admin_timeout(session);
        return false;
    }
    session->pending_tag = tag;
    session->pending_cli_sensitive = sensitive;
    session->pending_cli_read_only = read_only;
    session->pending_cli_reply_profile = reply_profile;
    session->request_deadline_us = request_deadline_us;
    session->cli_reply_valid = false;
    session->cli_reply_redacted = false;
    session->cli_reply_success = false;
    d1l_meshcore_admin_secure_zero(
        session->cli_reply, sizeof(session->cli_reply));
    session->state = D1L_MESHCORE_ADMIN_CLI_PENDING;
    session->generation = next_generation(session->generation);
    return true;
}

bool d1l_meshcore_admin_cancel_cli_command(
    d1l_meshcore_admin_session_t *session, uint32_t tag)
{
    if (!session || session->state != D1L_MESHCORE_ADMIN_CLI_PENDING ||
        session->pending_tag != tag) {
        return false;
    }
    session->pending_tag = 0U;
    session->pending_cli_sensitive = false;
    session->pending_cli_read_only = false;
    session->pending_cli_reply_profile =
        D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT;
    session->request_deadline_us = 0U;
    session->state = D1L_MESHCORE_ADMIN_AUTHENTICATED;
    session->generation = next_generation(session->generation);
    return true;
}

static bool cli_reply_byte_valid(uint8_t value)
{
    return value == '\n' || value == '\r' || value == '\t' ||
           (value >= 0x20U && value <= 0x7EU);
}

static bool cli_reply_prefix(
    const uint8_t *text, size_t text_len, size_t offset,
    const char *prefix)
{
    if (!text || !prefix || offset > text_len) {
        return false;
    }
    const size_t prefix_len = strlen(prefix);
    if (prefix_len > text_len - offset) {
        return false;
    }
    for (size_t i = 0U; i < prefix_len; ++i) {
        if (cli_ascii_lower(text[offset + i]) !=
            (uint8_t)prefix[i]) {
            return false;
        }
    }
    return true;
}

static bool cli_reply_contains(
    const uint8_t *text, size_t text_len, const char *needle)
{
    const size_t needle_len = needle ? strlen(needle) : 0U;
    if (!text || needle_len == 0U || needle_len > text_len) {
        return false;
    }
    for (size_t offset = 0U; offset + needle_len <= text_len; ++offset) {
        if (cli_reply_prefix(text, text_len, offset, needle)) {
            return true;
        }
    }
    return false;
}

static bool cli_reply_exact(
    const uint8_t *text, size_t text_len, size_t offset,
    const char *expected)
{
    if (!text || !expected || offset > text_len) {
        return false;
    }
    while (text_len > offset &&
           (text[text_len - 1U] == ' ' || text[text_len - 1U] == '\t' ||
            text[text_len - 1U] == '\r' || text[text_len - 1U] == '\n')) {
        text_len--;
    }
    return strlen(expected) == text_len - offset &&
           cli_reply_prefix(text, text_len, offset, expected);
}

static bool cli_reply_is_error(
    const uint8_t *text, size_t text_len, bool sensitive, bool read_only,
    d1l_meshcore_admin_cli_reply_profile_t reply_profile)
{
    if (!text || text_len == 0U) {
        return false;
    }
    size_t offset = 0U;
    while (offset < text_len &&
           (text[offset] == ' ' || text[offset] == '\t' ||
            text[offset] == '\r' || text[offset] == '\n')) {
        offset++;
    }
    if (offset < text_len && text[offset] == '(') {
        offset++;
    }

    /* A prompted read-only reply is a value, and several such values are
     * user-controlled. Never infer failure from words inside that payload. */
    bool read_only_value = false;
    if (read_only && offset < text_len && text[offset] == '>') {
        read_only_value = true;
        offset++;
        while (offset < text_len &&
               (text[offset] == ' ' || text[offset] == '\t')) {
            offset++;
        }
    }
    if ((reply_profile ==
             D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNKNOWN_VALUE &&
         cli_reply_exact(text, text_len, offset, "unknown")) ||
        (reply_profile ==
             D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNSUPPORTED_VALUE &&
         cli_reply_exact(text, text_len, offset, "unsupported")) ||
        (sensitive &&
         cli_reply_prefix(text, text_len, offset, "password now:"))) {
        return false;
    }
    if (read_only_value) {
        return false;
    }
    if (read_only) {
        switch (reply_profile) {
        case D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNKNOWN_VALUE:
            return cli_reply_exact(
                text, text_len, offset, "error: unsupported");
        case D1L_MESHCORE_ADMIN_CLI_REPLY_GET_VALUE:
            return cli_reply_prefix(text, text_len, offset, "??:");
        case D1L_MESHCORE_ADMIN_CLI_REPLY_ADC_UNSUPPORTED:
            return cli_reply_exact(
                text, text_len, offset,
                "error: unsupported by this board");
        case D1L_MESHCORE_ADMIN_CLI_REPLY_POWER_MANAGEMENT_UNSUPPORTED:
            return cli_reply_exact(
                text, text_len, offset,
                "error: power management not supported");
        case D1L_MESHCORE_ADMIN_CLI_REPLY_GPS_NOT_FOUND:
            return cli_reply_exact(
                text, text_len, offset, "can't find gps");
        case D1L_MESHCORE_ADMIN_CLI_REPLY_GPS_ADVERT_ERROR:
            return cli_reply_exact(text, text_len, offset, "error");
        case D1L_MESHCORE_ADMIN_CLI_REPLY_REGION_NOT_FOUND:
            return cli_reply_exact(
                text, text_len, offset, "err - unknown region");
        case D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT:
        case D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNSUPPORTED_VALUE:
        default:
            return false;
        }
    }
    static const char *const ERROR_PREFIXES[] = {
        "err", "error", "unknown", "??", "can't", "cannot",
    };
    for (size_t i = 0U;
         i < sizeof(ERROR_PREFIXES) / sizeof(ERROR_PREFIXES[0]); ++i) {
        if (cli_reply_prefix(
                text, text_len, offset, ERROR_PREFIXES[i])) {
            return true;
        }
    }
    static const char *const FAILURE_PHRASES[] = {
        " not found", " not supported", " unsupported",
        " failed", " invalid", " unable", " denied", " bad ",
        ": err",
    };
    for (size_t i = 0U;
         i < sizeof(FAILURE_PHRASES) / sizeof(FAILURE_PHRASES[0]); ++i) {
        if (cli_reply_contains(text, text_len, FAILURE_PHRASES[i])) {
            return true;
        }
    }
    return false;
}

d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_accept_cli_response(
    d1l_meshcore_admin_session_t *session,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    uint32_t response_timestamp, const uint8_t *text, size_t text_len,
    uint64_t now_us)
{
    if (!session || session->state != D1L_MESHCORE_ADMIN_CLI_PENDING ||
        !d1l_meshcore_admin_peer_matches(session, peer_public_key)) {
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    if (authenticated_deadline_due(session, now_us) ||
        deadline_due(session->request_deadline_us, now_us)) {
        d1l_meshcore_admin_timeout(session);
        return D1L_MESHCORE_ADMIN_RESPONSE_EXPIRED;
    }
    if (!text || text_len == 0U ||
        text_len > D1L_MESHCORE_ADMIN_MAX_CLI_REPLY_BYTES ||
        response_timestamp == 0U ||
        response_timestamp <= session->server_timestamp) {
        return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
    }
    for (size_t i = 0U; i < text_len; ++i) {
        if (!cli_reply_byte_valid(text[i])) {
            return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
        }
    }

    const bool rejected = cli_reply_is_error(
        text, text_len, session->pending_cli_sensitive,
        session->pending_cli_read_only,
        session->pending_cli_reply_profile);
    d1l_meshcore_admin_secure_zero(
        session->cli_reply, sizeof(session->cli_reply));
    if (session->pending_cli_sensitive) {
        static const char redacted[] = "[sensitive response hidden]";
        memcpy(session->cli_reply, redacted, sizeof(redacted));
        session->cli_reply_redacted = true;
    } else {
        memcpy(session->cli_reply, text, text_len);
        session->cli_reply[text_len] = '\0';
        session->cli_reply_redacted = false;
    }
    session->cli_reply_valid = true;
    session->cli_reply_success = !rejected;
    session->server_timestamp = response_timestamp;
    session->last_completed_tag = session->pending_tag;
    session->pending_tag = 0U;
    session->pending_cli_sensitive = false;
    session->pending_cli_read_only = false;
    session->pending_cli_reply_profile =
        D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT;
    session->request_deadline_us = 0U;
    refresh_idle_deadline(session, now_us);
    session->state = D1L_MESHCORE_ADMIN_AUTHENTICATED;
    session->generation = next_generation(session->generation);
    return rejected ? D1L_MESHCORE_ADMIN_RESPONSE_REJECTED :
                      D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED;
}

static bool mutation_error_reply(const uint8_t *text, size_t text_len)
{
    return text && text_len >= 3U &&
           ((memcmp(text, "ERR", 3U) == 0) ||
            (text_len >= 5U && memcmp(text, "Error", 5U) == 0) ||
            (text_len >= 4U && memcmp(text, "(ERR", 4U) == 0));
}

d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_accept_mutation_response(
    d1l_meshcore_admin_session_t *session,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    uint32_t response_timestamp, const uint8_t *text, size_t text_len,
    uint64_t now_us)
{
    if (!session ||
        session->state != D1L_MESHCORE_ADMIN_MUTATION_PENDING ||
        !d1l_meshcore_admin_peer_matches(session, peer_public_key)) {
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    if (authenticated_deadline_due(session, now_us) ||
        deadline_due(session->request_deadline_us, now_us)) {
        d1l_meshcore_admin_timeout(session);
        return D1L_MESHCORE_ADMIN_RESPONSE_EXPIRED;
    }
    if (!text || text_len == 0U ||
        text_len > D1L_MESHCORE_ADMIN_MUTATION_REPLY_MAX_BYTES ||
        response_timestamp == 0U ||
        response_timestamp <= session->server_timestamp) {
        return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
    }
    for (size_t i = 0U; i < text_len; ++i) {
        if (text[i] < 0x20U || text[i] > 0x7EU) {
            return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
        }
    }

    const d1l_meshcore_admin_mutation_t mutation =
        session->pending_mutation;
    const char *expected = mutation_success_reply(mutation);
    const size_t expected_len = expected ? strlen(expected) : 0U;
    const bool success = expected && text_len == expected_len &&
                         memcmp(text, expected, expected_len) == 0;
    const bool rejected = !success && mutation_error_reply(text, text_len);
    if (!success && !rejected) {
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }

    session->server_timestamp = response_timestamp;
    session->last_completed_tag = session->pending_tag;
    session->pending_tag = 0U;
    session->last_mutation = mutation;
    session->last_mutation_success = success;
    session->pending_mutation = D1L_MESHCORE_ADMIN_MUTATION_NONE;
    session->request_deadline_us = 0U;
    refresh_idle_deadline(session, now_us);
    session->state = D1L_MESHCORE_ADMIN_AUTHENTICATED;
    session->generation = next_generation(session->generation);
    return success ? D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED :
                     D1L_MESHCORE_ADMIN_RESPONSE_REJECTED;
}

size_t d1l_meshcore_admin_status_logical_size(
    d1l_meshcore_admin_role_t role)
{
    if (role == D1L_MESHCORE_ADMIN_ROLE_REPEATER) {
        return D1L_MESHCORE_ADMIN_REPEATER_STATUS_BYTES;
    }
    if (role == D1L_MESHCORE_ADMIN_ROLE_ROOM) {
        return D1L_MESHCORE_ADMIN_ROOM_STATUS_BYTES;
    }
    return 0U;
}

d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_accept_status_response(
    d1l_meshcore_admin_session_t *session,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t *plaintext, size_t plaintext_len, uint64_t now_us)
{
    if (!session || session->state != D1L_MESHCORE_ADMIN_STATUS_PENDING ||
        !d1l_meshcore_admin_peer_matches(session, peer_public_key)) {
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    if (authenticated_deadline_due(session, now_us) ||
        deadline_due(session->request_deadline_us, now_us)) {
        d1l_meshcore_admin_timeout(session);
        return D1L_MESHCORE_ADMIN_RESPONSE_EXPIRED;
    }
    const size_t logical_len =
        d1l_meshcore_admin_status_logical_size(session->role);
    if (logical_len == 0U ||
        !d1l_meshcore_admin_canonical_span(
            plaintext, plaintext_len, logical_len)) {
        return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
    }
    const uint32_t tag = read_le32(plaintext);
    if (tag != session->pending_tag || tag == session->last_completed_tag) {
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }

    d1l_meshcore_admin_status_t status = {0};
    size_t offset = 4U;
#define READ_U16(field)                                                        \
    do {                                                                       \
        status.field = read_le16(&plaintext[offset]);                          \
        offset += 2U;                                                          \
    } while (0)
#define READ_I16(field)                                                        \
    do {                                                                       \
        status.field = read_le_i16(&plaintext[offset]);                        \
        offset += 2U;                                                          \
    } while (0)
#define READ_U32(field)                                                        \
    do {                                                                       \
        status.field = read_le32(&plaintext[offset]);                          \
        offset += 4U;                                                          \
    } while (0)
    READ_U16(battery_millivolts);
    READ_U16(tx_queue_length);
    READ_I16(noise_floor_dbm);
    READ_I16(last_rssi_dbm);
    READ_U32(packets_received);
    READ_U32(packets_sent);
    READ_U32(tx_air_time_seconds);
    READ_U32(uptime_seconds);
    READ_U32(sent_flood);
    READ_U32(sent_direct);
    READ_U32(received_flood);
    READ_U32(received_direct);
    READ_U16(error_flags);
    READ_I16(last_snr_quarter_db);
    READ_U16(direct_duplicates);
    READ_U16(flood_duplicates);
    if (session->role == D1L_MESHCORE_ADMIN_ROLE_REPEATER) {
        READ_U32(rx_air_time_seconds);
        READ_U32(receive_errors);
    } else {
        READ_U16(posts_created);
        READ_U16(posts_pushed);
    }
#undef READ_U16
#undef READ_I16
#undef READ_U32
    if (offset != logical_len) {
        return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
    }

    session->status = status;
    session->status_valid = true;
    session->last_completed_tag = tag;
    session->pending_tag = 0U;
    session->request_deadline_us = 0U;
    refresh_idle_deadline(session, now_us);
    session->state = D1L_MESHCORE_ADMIN_AUTHENTICATED;
    session->generation = next_generation(session->generation);
    return D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED;
}

static bool bytes_all_zero(const uint8_t *data, size_t data_len)
{
    if (!data) {
        return false;
    }
    for (size_t i = 0U; i < data_len; ++i) {
        if (data[i] != 0U) {
            return false;
        }
    }
    return true;
}

static void query_append(d1l_meshcore_admin_query_result_t *result,
                         size_t *used, const char *format, ...)
{
    if (!result || !used || !format ||
        *used >= D1L_MESHCORE_ADMIN_MAX_QUERY_TEXT_BYTES) {
        if (result) {
            result->truncated = true;
        }
        return;
    }
    const size_t available =
        D1L_MESHCORE_ADMIN_MAX_QUERY_TEXT_BYTES + 1U - *used;
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(
        &result->text[*used], available, format, arguments);
    va_end(arguments);
    if (written < 0) {
        result->truncated = true;
        return;
    }
    if ((size_t)written >= available) {
        *used = D1L_MESHCORE_ADMIN_MAX_QUERY_TEXT_BYTES;
        result->text[*used] = '\0';
        result->truncated = true;
        return;
    }
    *used += (size_t)written;
}

static void format_scaled(char *out, size_t out_size, int32_t value,
                          uint32_t divisor, unsigned decimals)
{
    if (!out || out_size == 0U || divisor == 0U || decimals > 4U) {
        return;
    }
    const int64_t wide = (int64_t)value;
    const bool negative = wide < 0;
    const uint64_t magnitude =
        negative ? (uint64_t)(-wide) : (uint64_t)wide;
    const uint64_t whole = magnitude / divisor;
    const uint64_t fraction = magnitude % divisor;
    (void)snprintf(out, out_size, "%s%llu.%0*llu",
                   negative ? "-" : "",
                   (unsigned long long)whole, (int)decimals,
                   (unsigned long long)fraction);
}

static void format_scaled_unsigned(char *out, size_t out_size,
                                   uint32_t value, uint32_t divisor,
                                   unsigned decimals)
{
    if (!out || out_size == 0U || divisor == 0U || decimals > 4U) {
        return;
    }
    (void)snprintf(out, out_size, "%lu.%0*lu",
                   (unsigned long)(value / divisor), (int)decimals,
                   (unsigned long)(value % divisor));
}

static const char *permission_name(uint8_t permissions)
{
    switch (permissions & D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK) {
    case D1L_MESHCORE_ADMIN_PERMISSION_GUEST:
        return "guest";
    case D1L_MESHCORE_ADMIN_PERMISSION_READ_ONLY:
        return "read-only";
    case D1L_MESHCORE_ADMIN_PERMISSION_WRITE:
        return "read-write";
    case D1L_MESHCORE_ADMIN_PERMISSION_ADMIN:
        return "admin";
    default:
        return "invalid";
    }
}

static bool parse_telemetry_query(
    const uint8_t *data, size_t data_len,
    d1l_meshcore_admin_query_result_t *result)
{
    size_t offset = 4U;
    size_t used = 0U;
    query_append(result, &used, "Telemetry\n");
    while (offset < data_len) {
        const size_t remaining = data_len - offset;
        if (remaining <= D1L_MESHCORE_ADMIN_MAX_PADDING_BYTES &&
            bytes_all_zero(&data[offset], remaining)) {
            break;
        }
        if (remaining < 3U || data[offset] == 0U) {
            return false;
        }
        const uint8_t channel = data[offset];
        const uint8_t type = data[offset + 1U];
        const uint8_t *value = &data[offset + 2U];
        size_t value_len = 0U;
        switch (type) {
        case 0U:
        case 1U:
        case 102U:
        case 104U:
        case 120U:
        case 142U:
            value_len = 1U;
            break;
        case 2U:
        case 3U:
        case 101U:
        case 103U:
        case 115U:
        case 116U:
        case 117U:
        case 121U:
        case 125U:
        case 128U:
        case 132U:
            value_len = 2U;
            break;
        case 135U:
            value_len = 3U;
            break;
        case 100U:
        case 118U:
        case 130U:
        case 131U:
        case 133U:
            value_len = 4U;
            break;
        case 113U:
        case 134U:
            value_len = 6U;
            break;
        case 136U:
            value_len = 9U;
            break;
        case 240U:
            if (remaining < 3U || value[0] < 8U) {
                return false;
            }
            value_len = value[0];
            break;
        default:
            query_append(
                result, &used,
                "ch %u unsupported type %u (remaining data hidden)\n",
                (unsigned)channel, (unsigned)type);
            result->count++;
            result->truncated = true;
            return true;
        }
        if (remaining < 2U + value_len) {
            return false;
        }

        char first[32] = {0};
        char second[32] = {0};
        char third[32] = {0};
        switch (type) {
        case 0U:
            query_append(result, &used, "ch %u digital-in %u\n",
                         (unsigned)channel, (unsigned)value[0]);
            break;
        case 1U:
            query_append(result, &used, "ch %u digital-out %u\n",
                         (unsigned)channel, (unsigned)value[0]);
            break;
        case 2U:
        case 3U:
            format_scaled(first, sizeof(first), read_be_i16(value), 100U, 2U);
            query_append(result, &used, "ch %u %s %s\n",
                         (unsigned)channel,
                         type == 2U ? "analog-in" : "analog-out", first);
            break;
        case 100U:
            query_append(result, &used, "ch %u generic %lu\n",
                         (unsigned)channel,
                         (unsigned long)read_be32(value));
            break;
        case 101U:
            query_append(result, &used, "ch %u luminosity %u lux\n",
                         (unsigned)channel,
                         (unsigned)read_be16(value));
            break;
        case 102U:
            query_append(result, &used, "ch %u presence %u\n",
                         (unsigned)channel, (unsigned)value[0]);
            break;
        case 103U:
            format_scaled(first, sizeof(first), read_be_i16(value), 10U, 1U);
            query_append(result, &used, "ch %u temperature %s C\n",
                         (unsigned)channel, first);
            break;
        case 104U:
            (void)snprintf(
                first, sizeof(first), "%u.%u",
                (unsigned)(value[0] / 2U),
                (unsigned)((value[0] % 2U) * 5U));
            query_append(result, &used, "ch %u humidity %s %%\n",
                         (unsigned)channel, first);
            break;
        case 113U:
        case 134U:
            format_scaled(
                first, sizeof(first), read_be_i16(value),
                type == 113U ? 1000U : 100U,
                type == 113U ? 3U : 2U);
            format_scaled(
                second, sizeof(second), read_be_i16(&value[2]),
                type == 113U ? 1000U : 100U,
                type == 113U ? 3U : 2U);
            format_scaled(
                third, sizeof(third), read_be_i16(&value[4]),
                type == 113U ? 1000U : 100U,
                type == 113U ? 3U : 2U);
            query_append(result, &used, "ch %u %s %s,%s,%s\n",
                         (unsigned)channel,
                         type == 113U ? "accel" : "gyro",
                         first, second, third);
            break;
        case 115U:
            format_scaled_unsigned(
                first, sizeof(first), read_be16(value), 10U, 1U);
            query_append(result, &used, "ch %u pressure %s hPa\n",
                         (unsigned)channel, first);
            break;
        case 116U:
            format_scaled_unsigned(
                first, sizeof(first), read_be16(value), 100U, 2U);
            query_append(result, &used, "ch %u voltage %s V\n",
                         (unsigned)channel, first);
            break;
        case 117U:
            format_scaled(
                first, sizeof(first), read_be_i16(value), 1000U, 3U);
            query_append(result, &used, "ch %u current %s A\n",
                         (unsigned)channel, first);
            break;
        case 118U:
            query_append(result, &used, "ch %u frequency %lu Hz\n",
                         (unsigned)channel,
                         (unsigned long)read_be32(value));
            break;
        case 120U:
            query_append(result, &used, "ch %u percentage %u %%\n",
                         (unsigned)channel, (unsigned)value[0]);
            break;
        case 121U:
            query_append(result, &used, "ch %u altitude %d m\n",
                         (unsigned)channel, (int)read_be_i16(value));
            break;
        case 125U:
            query_append(result, &used, "ch %u concentration %u ppm\n",
                         (unsigned)channel,
                         (unsigned)read_be16(value));
            break;
        case 128U:
            query_append(result, &used, "ch %u power %u W\n",
                         (unsigned)channel,
                         (unsigned)read_be16(value));
            break;
        case 130U:
        case 131U:
            format_scaled_unsigned(
                first, sizeof(first), read_be32(value), 1000U, 3U);
            query_append(result, &used, "ch %u %s %s %s\n",
                         (unsigned)channel,
                         type == 130U ? "distance" : "energy", first,
                         type == 130U ? "m" : "kWh");
            break;
        case 132U:
            query_append(result, &used, "ch %u direction %u deg\n",
                         (unsigned)channel,
                         (unsigned)read_be16(value));
            break;
        case 133U:
            query_append(result, &used, "ch %u unix-time %lu\n",
                         (unsigned)channel,
                         (unsigned long)read_be32(value));
            break;
        case 135U:
            query_append(result, &used, "ch %u RGB #%02X%02X%02X\n",
                         (unsigned)channel, (unsigned)value[0],
                         (unsigned)value[1], (unsigned)value[2]);
            break;
        case 136U:
            format_scaled(
                first, sizeof(first), read_be_i24(value), 10000U, 4U);
            format_scaled(
                second, sizeof(second), read_be_i24(&value[3]), 10000U, 4U);
            format_scaled(
                third, sizeof(third), read_be_i24(&value[6]), 100U, 2U);
            query_append(result, &used, "ch %u GPS %s,%s alt %s m\n",
                         (unsigned)channel, first, second, third);
            break;
        case 142U:
            query_append(result, &used, "ch %u switch %u\n",
                         (unsigned)channel, (unsigned)value[0]);
            break;
        case 240U:
            query_append(result, &used, "ch %u polyline %u bytes\n",
                         (unsigned)channel, (unsigned)value_len);
            break;
        default:
            return false;
        }
        result->count++;
        offset += 2U + value_len;
    }
    if (result->count == 0U) {
        query_append(result, &used, "No telemetry fields returned.\n");
    }
    result->total = result->count;
    return true;
}

bool d1l_meshcore_telemetry_decode(
    const uint8_t *plaintext, size_t plaintext_len,
    d1l_meshcore_admin_query_result_t *out_result)
{
    if (!plaintext || plaintext_len < 4U || !out_result) {
        return false;
    }
    d1l_meshcore_admin_query_result_t parsed = {
        .kind = D1L_MESHCORE_ADMIN_QUERY_TELEMETRY,
        .valid = true,
    };
    if (!parse_telemetry_query(plaintext, plaintext_len, &parsed)) {
        d1l_meshcore_admin_secure_zero(&parsed, sizeof(parsed));
        return false;
    }
    *out_result = parsed;
    d1l_meshcore_admin_secure_zero(&parsed, sizeof(parsed));
    return true;
}

static bool parse_access_list_query(
    const uint8_t *data, size_t data_len,
    d1l_meshcore_admin_query_result_t *result)
{
    size_t offset = 4U;
    size_t used = 0U;
    query_append(result, &used, "Access list\n");
    while (offset < data_len) {
        const size_t remaining = data_len - offset;
        if (remaining <= D1L_MESHCORE_ADMIN_MAX_PADDING_BYTES &&
            bytes_all_zero(&data[offset], remaining)) {
            break;
        }
        if (remaining < 7U) {
            return false;
        }
        const uint8_t *entry = &data[offset];
        query_append(
            result, &used,
            "%02X%02X%02X%02X%02X%02X  %s (0x%02X)\n",
            (unsigned)entry[0], (unsigned)entry[1], (unsigned)entry[2],
            (unsigned)entry[3], (unsigned)entry[4], (unsigned)entry[5],
            permission_name(entry[6]), (unsigned)entry[6]);
        result->count++;
        offset += 7U;
    }
    if (result->count == 0U) {
        query_append(result, &used, "No access-list entries returned.\n");
    }
    result->total = result->count;
    return true;
}

static bool parse_neighbours_query(
    const uint8_t *data, size_t data_len, uint16_t requested_offset,
    d1l_meshcore_admin_query_result_t *result)
{
    if (data_len < 8U) {
        return false;
    }
    const uint16_t total = read_le16(&data[4]);
    const uint16_t count = read_le16(&data[6]);
    if (count > D1L_MESHCORE_ADMIN_NEIGHBOUR_PAGE_COUNT ||
        count > total ||
        requested_offset > total ||
        count > (uint16_t)(total - requested_offset)) {
        return false;
    }
    const size_t entry_size =
        D1L_MESHCORE_ADMIN_NEIGHBOUR_PREFIX_BYTES + 4U + 1U;
    const size_t logical_len = 8U + (size_t)count * entry_size;
    if (!d1l_meshcore_admin_canonical_span(
            data, data_len, logical_len)) {
        return false;
    }

    result->offset = requested_offset;
    result->total = total;
    result->count = count;
    size_t used = 0U;
    query_append(result, &used, "Neighbours %u-%u of %u\n",
                 (unsigned)(count == 0U ? requested_offset :
                            requested_offset + 1U),
                 (unsigned)(requested_offset + count),
                 (unsigned)total);
    size_t offset = 8U;
    for (uint16_t index = 0U; index < count; ++index) {
        const uint8_t *entry = &data[offset];
        const uint32_t seconds_ago =
            read_le32(&entry[D1L_MESHCORE_ADMIN_NEIGHBOUR_PREFIX_BYTES]);
        const int8_t snr_quarter_db =
            (int8_t)entry[D1L_MESHCORE_ADMIN_NEIGHBOUR_PREFIX_BYTES + 4U];
        const int snr_magnitude =
            snr_quarter_db < 0 ? -(int)snr_quarter_db :
                                 (int)snr_quarter_db;
        const int snr_whole = snr_magnitude / 4;
        const int snr_fraction = snr_magnitude % 4;
        query_append(
            result, &used, "%02X%02X%02X%02X  %lus ago  %s%d.%02d dB\n",
            (unsigned)entry[0], (unsigned)entry[1],
            (unsigned)entry[2], (unsigned)entry[3],
            (unsigned long)seconds_ago,
            snr_quarter_db < 0 ? "-" : "", snr_whole,
            snr_fraction * 25);
        offset += entry_size;
    }
    if (count == 0U) {
        query_append(result, &used, "No neighbours on this page.\n");
    }
    return true;
}

d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_accept_query_response(
    d1l_meshcore_admin_session_t *session,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t *plaintext, size_t plaintext_len, uint64_t now_us)
{
    if (!session || session->state != D1L_MESHCORE_ADMIN_QUERY_PENDING ||
        !d1l_meshcore_admin_peer_matches(session, peer_public_key)) {
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    if (authenticated_deadline_due(session, now_us) ||
        deadline_due(session->request_deadline_us, now_us)) {
        d1l_meshcore_admin_timeout(session);
        return D1L_MESHCORE_ADMIN_RESPONSE_EXPIRED;
    }
    if (!plaintext || plaintext_len < 4U) {
        return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
    }
    const uint32_t tag = read_le32(plaintext);
    if (tag != session->pending_tag || tag == session->last_completed_tag) {
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }

    d1l_meshcore_admin_query_result_t parsed = {
        .kind = session->pending_query,
        .valid = true,
        .offset = session->pending_query_offset,
    };
    bool valid = false;
    switch (session->pending_query) {
    case D1L_MESHCORE_ADMIN_QUERY_TELEMETRY:
        valid = d1l_meshcore_telemetry_decode(
            plaintext, plaintext_len, &parsed);
        break;
    case D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST:
        valid = parse_access_list_query(plaintext, plaintext_len, &parsed);
        break;
    case D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS:
        valid = parse_neighbours_query(
            plaintext, plaintext_len, session->pending_query_offset,
            &parsed);
        break;
    case D1L_MESHCORE_ADMIN_QUERY_NONE:
    default:
        break;
    }
    if (!valid) {
        d1l_meshcore_admin_secure_zero(&parsed, sizeof(parsed));
        return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
    }

    session->query_result = parsed;
    session->last_completed_tag = tag;
    session->pending_tag = 0U;
    session->pending_query = D1L_MESHCORE_ADMIN_QUERY_NONE;
    session->pending_query_offset = 0U;
    session->request_deadline_us = 0U;
    refresh_idle_deadline(session, now_us);
    session->state = D1L_MESHCORE_ADMIN_AUTHENTICATED;
    session->generation = next_generation(session->generation);
    d1l_meshcore_admin_secure_zero(&parsed, sizeof(parsed));
    return D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED;
}

bool d1l_meshcore_admin_expire_if_due(
    d1l_meshcore_admin_session_t *session, uint64_t now_us)
{
    if (!session) {
        return false;
    }
    bool due = false;
    if (session->state == D1L_MESHCORE_ADMIN_LOGIN_PENDING) {
        due = deadline_due(session->request_deadline_us, now_us);
    } else if (session->state == D1L_MESHCORE_ADMIN_AUTHENTICATED) {
        due = authenticated_deadline_due(session, now_us);
    } else if (session->state == D1L_MESHCORE_ADMIN_STATUS_PENDING ||
               session->state == D1L_MESHCORE_ADMIN_MUTATION_PENDING ||
               session->state == D1L_MESHCORE_ADMIN_CLI_PENDING ||
               session->state == D1L_MESHCORE_ADMIN_QUERY_PENDING) {
        due = authenticated_deadline_due(session, now_us) ||
              deadline_due(session->request_deadline_us, now_us);
    }
    if (!due) {
        return false;
    }
    d1l_meshcore_admin_timeout(session);
    return true;
}
