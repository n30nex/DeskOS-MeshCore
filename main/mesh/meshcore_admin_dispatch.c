#include "meshcore_admin_dispatch.h"

#include <limits.h>
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
    if (!session) {
        return;
    }
    const uint32_t generation = next_generation(session->generation);
    d1l_meshcore_admin_secure_zero(session, sizeof(*session));
    session->state = D1L_MESHCORE_ADMIN_TIMED_OUT;
    session->generation = generation;
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
         session->state != D1L_MESHCORE_ADMIN_TIMED_OUT)) {
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
    const uint8_t response[D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES])
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
    const uint8_t response[D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES])
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
        return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
    }

    const uint8_t permissions = plaintext[7];
    const uint8_t expected_firmware =
        session->role == D1L_MESHCORE_ADMIN_ROLE_REPEATER ? 2U : 1U;
    if (plaintext[4] != 0U || plaintext[5] != 0U || plaintext[6] != 1U ||
        (permissions & D1L_MESHCORE_ADMIN_PERMISSION_ADMIN) !=
            D1L_MESHCORE_ADMIN_PERMISSION_ADMIN ||
        plaintext[12] != expected_firmware) {
        return D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED;
    }
    if (replay_cache_contains(replay_cache, peer_public_key, plaintext)) {
        d1l_meshcore_admin_timeout(session);
        return D1L_MESHCORE_ADMIN_RESPONSE_REPLAYED;
    }

    replay_cache_remember(replay_cache, peer_public_key, plaintext);
    session->server_timestamp = read_le32(plaintext);
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
               session->state == D1L_MESHCORE_ADMIN_MUTATION_PENDING) {
        due = authenticated_deadline_due(session, now_us) ||
              deadline_due(session->request_deadline_us, now_us);
    }
    if (!due) {
        return false;
    }
    d1l_meshcore_admin_timeout(session);
    return true;
}
