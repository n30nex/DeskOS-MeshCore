#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES 32U
#define D1L_MESHCORE_ADMIN_SECRET_BYTES 32U
#define D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES 15U
#define D1L_MESHCORE_ADMIN_REPEATER_LOGIN_PREFIX_BYTES 4U
#define D1L_MESHCORE_ADMIN_ROOM_LOGIN_PREFIX_BYTES 8U
#define D1L_MESHCORE_ADMIN_MAX_LOGIN_REQUEST_BYTES \
    (D1L_MESHCORE_ADMIN_ROOM_LOGIN_PREFIX_BYTES + \
     D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES)
#define D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES 13U
#define D1L_MESHCORE_ADMIN_REQUEST_BYTES 13U
#define D1L_MESHCORE_ADMIN_REPEATER_STATUS_BYTES 60U
#define D1L_MESHCORE_ADMIN_ROOM_STATUS_BYTES 56U
#define D1L_MESHCORE_ADMIN_MAX_PADDING_BYTES 15U
#define D1L_MESHCORE_ADMIN_REPLAY_PEER_CAPACITY 8U
#define D1L_MESHCORE_ADMIN_REPLAY_RESPONSES_PER_PEER 4U
#define D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK 0x03U
#define D1L_MESHCORE_ADMIN_PERMISSION_GUEST 0x00U
#define D1L_MESHCORE_ADMIN_PERMISSION_READ_ONLY 0x01U
#define D1L_MESHCORE_ADMIN_PERMISSION_WRITE 0x02U
#define D1L_MESHCORE_ADMIN_PERMISSION_ADMIN 0x03U
#define D1L_MESHCORE_ADMIN_REQUEST_GET_STATUS 0x01U
#define D1L_MESHCORE_ADMIN_REQUEST_GET_TELEMETRY 0x03U
#define D1L_MESHCORE_ADMIN_REQUEST_GET_ACCESS_LIST 0x05U
#define D1L_MESHCORE_ADMIN_REQUEST_GET_NEIGHBOURS 0x06U
#define D1L_MESHCORE_ADMIN_MAX_QUERY_REQUEST_BYTES 15U
#define D1L_MESHCORE_ADMIN_NEIGHBOUR_PAGE_COUNT 10U
#define D1L_MESHCORE_ADMIN_NEIGHBOUR_PREFIX_BYTES 4U
#define D1L_MESHCORE_ADMIN_MAX_QUERY_TEXT_BYTES 768U
#define D1L_MESHCORE_ADMIN_MUTATION_REPLY_MAX_BYTES 64U
#define D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES 160U
#define D1L_MESHCORE_ADMIN_MAX_CLI_REPLY_BYTES 160U
#define D1L_MESHCORE_ADMIN_ACL_EDIT_MAX_BYTES \
    ((D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES * 2U) + 2U)
/* Room login uses the current request timestamp as its sync cursor. This
 * starts a live current-session console without replaying older posts. */

typedef enum {
    D1L_MESHCORE_ADMIN_ROLE_NONE = 0,
    D1L_MESHCORE_ADMIN_ROLE_REPEATER,
    D1L_MESHCORE_ADMIN_ROLE_ROOM,
} d1l_meshcore_admin_role_t;

typedef enum {
    D1L_MESHCORE_ADMIN_IDLE = 0,
    D1L_MESHCORE_ADMIN_LOGIN_PENDING,
    D1L_MESHCORE_ADMIN_AUTHENTICATED,
    D1L_MESHCORE_ADMIN_STATUS_PENDING,
    D1L_MESHCORE_ADMIN_MUTATION_PENDING,
    D1L_MESHCORE_ADMIN_CLI_PENDING,
    D1L_MESHCORE_ADMIN_QUERY_PENDING,
    D1L_MESHCORE_ADMIN_TIMED_OUT,
    D1L_MESHCORE_ADMIN_REJECTED_CREDENTIALS,
    D1L_MESHCORE_ADMIN_DISCONNECTED,
    D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL,
    D1L_MESHCORE_ADMIN_RADIO_BUSY,
} d1l_meshcore_admin_state_t;

typedef enum {
    D1L_MESHCORE_ADMIN_MUTATION_NONE = 0,
    D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS,
    D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP,
} d1l_meshcore_admin_mutation_t;

typedef enum {
    D1L_MESHCORE_ADMIN_QUERY_NONE = 0,
    D1L_MESHCORE_ADMIN_QUERY_TELEMETRY,
    D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST,
    D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS,
} d1l_meshcore_admin_query_t;

typedef enum {
    D1L_MESHCORE_ADMIN_CLI_UNSUPPORTED = 0,
    D1L_MESHCORE_ADMIN_CLI_READ_ONLY,
    D1L_MESHCORE_ADMIN_CLI_MUTATION,
    D1L_MESHCORE_ADMIN_CLI_SENSITIVE,
} d1l_meshcore_admin_cli_policy_t;

typedef enum {
    D1L_MESHCORE_ADMIN_CLI_REPLY_DEFAULT = 0,
    D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNKNOWN_VALUE,
    D1L_MESHCORE_ADMIN_CLI_REPLY_PROMPT_UNSUPPORTED_VALUE,
} d1l_meshcore_admin_cli_reply_profile_t;

typedef enum {
    D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED = 0,
    D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED,
    D1L_MESHCORE_ADMIN_RESPONSE_REJECTED,
    D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED,
    D1L_MESHCORE_ADMIN_RESPONSE_EXPIRED,
    D1L_MESHCORE_ADMIN_RESPONSE_REPLAYED,
} d1l_meshcore_admin_response_result_t;

typedef struct {
    bool valid;
    uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES];
    uint8_t responses[D1L_MESHCORE_ADMIN_REPLAY_RESPONSES_PER_PEER]
                     [D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES];
    uint32_t highest_server_timestamp;
    uint8_t response_count;
    uint8_t next_response;
} d1l_meshcore_admin_replay_entry_t;

/* This cache deliberately lives outside d1l_meshcore_admin_session_t. A
 * logout, timeout, failed transmit, or new login attempt must not make any of
 * the four most recently accepted responses for that peer acceptable again.
 * The per-peer server timestamp high-water also rejects responses evicted from
 * that bounded response ring. Runtime code must bind this volatile guard to
 * durable per-full-key storage before exposing authenticated authority. */
typedef struct {
    d1l_meshcore_admin_replay_entry_t
        peers[D1L_MESHCORE_ADMIN_REPLAY_PEER_CAPACITY];
    uint8_t next_replacement;
} d1l_meshcore_admin_replay_cache_t;

typedef struct {
    uint16_t battery_millivolts;
    uint16_t tx_queue_length;
    int16_t noise_floor_dbm;
    int16_t last_rssi_dbm;
    uint32_t packets_received;
    uint32_t packets_sent;
    uint32_t tx_air_time_seconds;
    uint32_t uptime_seconds;
    uint32_t sent_flood;
    uint32_t sent_direct;
    uint32_t received_flood;
    uint32_t received_direct;
    uint16_t error_flags;
    int16_t last_snr_quarter_db;
    uint16_t direct_duplicates;
    uint16_t flood_duplicates;
    uint32_t rx_air_time_seconds;
    uint32_t receive_errors;
    uint16_t posts_created;
    uint16_t posts_pushed;
} d1l_meshcore_admin_status_t;

typedef struct {
    d1l_meshcore_admin_query_t kind;
    bool valid;
    bool truncated;
    uint16_t offset;
    uint16_t total;
    uint16_t count;
    char text[D1L_MESHCORE_ADMIN_MAX_QUERY_TEXT_BYTES + 1U];
} d1l_meshcore_admin_query_result_t;

/* Runtime-only client authority. The service owns this structure behind its
 * lock. It must never be copied into diagnostics, retained stores, or UI
 * snapshots because it contains the active ECDH session secret. */
typedef struct {
    d1l_meshcore_admin_state_t state;
    d1l_meshcore_admin_role_t role;
    uint32_t generation;
    uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES];
    uint8_t local_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES];
    uint8_t session_secret[D1L_MESHCORE_ADMIN_SECRET_BYTES];
    uint8_t permissions;
    uint8_t firmware_level;
    uint32_t server_timestamp;
    uint32_t pending_tag;
    uint32_t last_completed_tag;
    d1l_meshcore_admin_mutation_t pending_mutation;
    d1l_meshcore_admin_mutation_t last_mutation;
    d1l_meshcore_admin_query_t pending_query;
    uint16_t pending_query_offset;
    bool last_mutation_success;
    bool pending_cli_sensitive;
    bool pending_cli_read_only;
    d1l_meshcore_admin_cli_reply_profile_t pending_cli_reply_profile;
    bool cli_reply_valid;
    bool cli_reply_redacted;
    bool cli_reply_success;
    char cli_reply[D1L_MESHCORE_ADMIN_MAX_CLI_REPLY_BYTES + 1U];
    uint64_t request_deadline_us;
    uint64_t idle_timeout_us;
    uint64_t idle_deadline_us;
    uint64_t absolute_timeout_us;
    uint64_t absolute_deadline_us;
    bool status_valid;
    d1l_meshcore_admin_status_t status;
    d1l_meshcore_admin_query_result_t query_result;
} d1l_meshcore_admin_session_t;

void d1l_meshcore_admin_secure_zero(void *value, size_t size);
bool d1l_meshcore_admin_encode_login_request(
    d1l_meshcore_admin_role_t role, uint32_t timestamp,
    uint32_t room_sync_since,
    const uint8_t *password, size_t password_len, uint8_t *out,
    size_t out_size, size_t *out_len);
bool d1l_meshcore_admin_encode_status_request(
    uint32_t tag, uint32_t uniqueness,
    uint8_t out[D1L_MESHCORE_ADMIN_REQUEST_BYTES]);
const char *d1l_meshcore_admin_query_name(
    d1l_meshcore_admin_query_t query);
bool d1l_meshcore_admin_query_allowed(
    d1l_meshcore_admin_role_t role, uint8_t permissions,
    d1l_meshcore_admin_query_t query);
bool d1l_meshcore_admin_encode_query_request(
    d1l_meshcore_admin_query_t query, uint32_t tag, uint16_t offset,
    uint32_t uniqueness, uint8_t *out, size_t out_size, size_t *out_len);
void d1l_meshcore_admin_reset(d1l_meshcore_admin_session_t *session);
void d1l_meshcore_admin_timeout(d1l_meshcore_admin_session_t *session);
bool d1l_meshcore_admin_fail(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_state_t failure_state);
void d1l_meshcore_admin_replay_cache_clear(
    d1l_meshcore_admin_replay_cache_t *cache);
bool d1l_meshcore_admin_begin_login(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_role_t role,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t local_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t session_secret[D1L_MESHCORE_ADMIN_SECRET_BYTES],
    uint64_t request_deadline_us, uint64_t idle_timeout_us,
    uint64_t absolute_timeout_us);
bool d1l_meshcore_admin_peer_matches(
    const d1l_meshcore_admin_session_t *session,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES]);
bool d1l_meshcore_admin_binding_matches(
    const d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_role_t role,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t local_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t session_secret[D1L_MESHCORE_ADMIN_SECRET_BYTES]);
bool d1l_meshcore_admin_note_authenticated_activity(
    d1l_meshcore_admin_session_t *session, uint64_t now_us);
bool d1l_meshcore_admin_canonical_span(const uint8_t *data, size_t data_len,
                                       size_t logical_len);
d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_accept_login_response(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_replay_cache_t *replay_cache,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t *plaintext, size_t plaintext_len, uint64_t now_us);
bool d1l_meshcore_admin_begin_status_request(
    d1l_meshcore_admin_session_t *session, uint32_t tag,
    uint64_t now_us, uint64_t request_deadline_us);
bool d1l_meshcore_admin_cancel_status_request(
    d1l_meshcore_admin_session_t *session, uint32_t tag);
bool d1l_meshcore_admin_begin_query_request(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_query_t query, uint16_t offset, uint32_t tag,
    uint64_t now_us, uint64_t request_deadline_us);
bool d1l_meshcore_admin_cancel_query_request(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_query_t query, uint32_t tag);
const char *d1l_meshcore_admin_mutation_name(
    d1l_meshcore_admin_mutation_t mutation);
const char *d1l_meshcore_admin_mutation_command(
    d1l_meshcore_admin_mutation_t mutation);
bool d1l_meshcore_admin_begin_mutation(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_mutation_t mutation, uint32_t tag,
    uint64_t now_us, uint64_t request_deadline_us);
bool d1l_meshcore_admin_cancel_mutation(
    d1l_meshcore_admin_session_t *session,
    d1l_meshcore_admin_mutation_t mutation, uint32_t tag);
d1l_meshcore_admin_cli_policy_t d1l_meshcore_admin_cli_command_policy(
    const char *command);
bool d1l_meshcore_admin_cli_command_valid(const char *command);
bool d1l_meshcore_admin_cli_command_sensitive(const char *command);
bool d1l_meshcore_admin_cli_command_read_only(const char *command);
d1l_meshcore_admin_cli_reply_profile_t
d1l_meshcore_admin_cli_command_reply_profile(const char *command);
bool d1l_meshcore_admin_cli_command_allowed(
    const char *command, d1l_meshcore_admin_role_t role,
    uint8_t permissions);
bool d1l_meshcore_admin_format_acl_command(
    const char *full_public_key_hex, uint8_t permissions,
    char *out_command, size_t out_command_size);
bool d1l_meshcore_admin_begin_cli_command(
    d1l_meshcore_admin_session_t *session, uint32_t tag,
    bool sensitive, bool read_only,
    d1l_meshcore_admin_cli_reply_profile_t reply_profile, uint64_t now_us,
    uint64_t request_deadline_us);
bool d1l_meshcore_admin_cancel_cli_command(
    d1l_meshcore_admin_session_t *session, uint32_t tag);
d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_accept_cli_response(
    d1l_meshcore_admin_session_t *session,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    uint32_t response_timestamp, const uint8_t *text, size_t text_len,
    uint64_t now_us);
d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_accept_mutation_response(
    d1l_meshcore_admin_session_t *session,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    uint32_t response_timestamp, const uint8_t *text, size_t text_len,
    uint64_t now_us);
size_t d1l_meshcore_admin_status_logical_size(
    d1l_meshcore_admin_role_t role);
d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_accept_status_response(
    d1l_meshcore_admin_session_t *session,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t *plaintext, size_t plaintext_len, uint64_t now_us);
d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_accept_query_response(
    d1l_meshcore_admin_session_t *session,
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t *plaintext, size_t plaintext_len, uint64_t now_us);
/*
 * Decode one authenticated MeshCore telemetry response whose first four
 * bytes are the caller's correlation tag.  This pure formatter is shared by
 * authenticated server sessions and ordinary contact path-discovery
 * responses; it never retains the plaintext or any session secret.
 */
bool d1l_meshcore_telemetry_decode(
    const uint8_t *plaintext, size_t plaintext_len,
    d1l_meshcore_admin_query_result_t *out_result);
bool d1l_meshcore_admin_expire_if_due(
    d1l_meshcore_admin_session_t *session, uint64_t now_us);

#ifdef __cplusplus
}
#endif
