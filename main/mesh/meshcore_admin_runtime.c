#include "meshcore_admin_runtime.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "mesh/meshcore_wire.h"
#include "mesh/store_lock.h"
#include "nvs.h"

#define D1L_MESHCORE_ADMIN_ANON_REQUEST_TYPE 0x07U
#define D1L_MESHCORE_ADMIN_REQUEST_TYPE 0x00U
#define D1L_MESHCORE_TXT_TYPE_CLI_DATA 0x01U
#define D1L_MESHCORE_ADMIN_REPLAY_NAMESPACE "d1l_admin"
#define D1L_MESHCORE_ADMIN_REPLAY_MAGIC UINT32_C(0x41444D52)
#define D1L_MESHCORE_ADMIN_REPLAY_VERSION UINT32_C(1)

typedef struct {
    uint32_t login_tx_queued;
    uint32_t status_tx_queued;
    uint32_t query_tx_queued;
    uint32_t query_accepted;
    uint32_t query_rejected;
    uint32_t mutation_tx_queued;
    uint32_t mutation_accepted;
    uint32_t mutation_rejected;
    uint32_t cli_tx_queued;
    uint32_t cli_accepted;
    uint32_t cli_rejected;
    uint32_t response_accepted;
    uint32_t response_unmatched;
    uint32_t response_malformed;
    uint32_t response_expired;
    uint32_t response_replayed;
    esp_err_t last_error;
} d1l_meshcore_admin_metrics_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES];
    uint32_t highest_server_timestamp;
    uint8_t last_response[D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES];
} d1l_meshcore_admin_durable_replay_record_t;

static d1l_meshcore_admin_session_t s_session;
static d1l_meshcore_admin_replay_cache_t s_replay_cache;
static char s_fingerprint[D1L_NODE_FINGERPRINT_LEN];
static d1l_meshcore_admin_metrics_t s_metrics;
static d1l_store_lock_t s_lock = D1L_STORE_LOCK_INITIALIZER;

static uint64_t deadline_after(uint64_t now_us, uint64_t timeout_us)
{
    return now_us > UINT64_MAX - timeout_us ? UINT64_MAX
                                             : now_us + timeout_us;
}

static void write_le32(uint8_t *dest, uint32_t value)
{
    dest[0] = (uint8_t)value;
    dest[1] = (uint8_t)(value >> 8U);
    dest[2] = (uint8_t)(value >> 16U);
    dest[3] = (uint8_t)(value >> 24U);
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool hex_to_key(uint8_t *dest, size_t dest_size, const char *hex)
{
    if (!dest || !hex || strlen(hex) != dest_size * 2U) {
        return false;
    }
    for (size_t i = 0U; i < dest_size; ++i) {
        const int high = hex_nibble(hex[i * 2U]);
        const int low = hex_nibble(hex[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            d1l_meshcore_admin_secure_zero(dest, dest_size);
            return false;
        }
        dest[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool same_bytes(const uint8_t *left, const uint8_t *right, size_t size)
{
    if (!left || !right) {
        return false;
    }
    uint8_t difference = 0U;
    for (size_t i = 0U; i < size; ++i) {
        difference |= (uint8_t)(left[i] ^ right[i]);
    }
    return difference == 0U;
}

static bool durable_replay_key(
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    char out_key[16])
{
    if (!peer_public_key || !out_key) {
        return false;
    }
    const int written = snprintf(
        out_key, 16U, "p%02x%02x%02x%02x%02x%02x%02x",
        peer_public_key[0], peer_public_key[1], peer_public_key[2],
        peer_public_key[3], peer_public_key[4], peer_public_key[5],
        peer_public_key[6]);
    return written == 15;
}

static esp_err_t durable_replay_commit(
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t response[D1L_MESHCORE_ADMIN_LOGIN_RESPONSE_BYTES],
    uint32_t server_timestamp)
{
    if (!peer_public_key || !response || server_timestamp == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[16] = {0};
    if (!durable_replay_key(peer_public_key, key)) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle = 0U;
    esp_err_t ret = nvs_open(
        D1L_MESHCORE_ADMIN_REPLAY_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_meshcore_admin_durable_replay_record_t record = {0};
    size_t record_size = sizeof(record);
    ret = nvs_get_blob(handle, key, &record, &record_size);
    if (ret == ESP_OK) {
        if (record_size != sizeof(record) ||
            record.magic != D1L_MESHCORE_ADMIN_REPLAY_MAGIC ||
            record.version != D1L_MESHCORE_ADMIN_REPLAY_VERSION ||
            !same_bytes(
                record.peer_public_key, peer_public_key,
                D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES)) {
            ret = ESP_ERR_INVALID_STATE;
        } else if (server_timestamp <= record.highest_server_timestamp) {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }

    if (ret == ESP_OK) {
        memset(&record, 0, sizeof(record));
        record.magic = D1L_MESHCORE_ADMIN_REPLAY_MAGIC;
        record.version = D1L_MESHCORE_ADMIN_REPLAY_VERSION;
        memcpy(record.peer_public_key, peer_public_key,
               sizeof(record.peer_public_key));
        record.highest_server_timestamp = server_timestamp;
        memcpy(record.last_response, response, sizeof(record.last_response));
        ret = nvs_set_blob(handle, key, &record, sizeof(record));
        if (ret == ESP_OK) {
            ret = nvs_commit(handle);
        }
    }
    d1l_meshcore_admin_secure_zero(&record, sizeof(record));
    nvs_close(handle);
    return ret;
}

d1l_meshcore_admin_role_t d1l_meshcore_admin_role_for_contact(
    const d1l_contact_entry_t *contact)
{
    if (!contact || !d1l_contact_store_can_admin(contact)) {
        return D1L_MESHCORE_ADMIN_ROLE_NONE;
    }
    if (strcmp(contact->type, "repeater") == 0) {
        return D1L_MESHCORE_ADMIN_ROLE_REPEATER;
    }
    if (strcmp(contact->type, "room") == 0) {
        return D1L_MESHCORE_ADMIN_ROLE_ROOM;
    }
    return D1L_MESHCORE_ADMIN_ROLE_NONE;
}

bool d1l_meshcore_admin_route_valid(
    const d1l_meshcore_route_selection_t *selection)
{
    if (!selection ||
        (selection->route != D1L_MESHCORE_ROUTE_DIRECT &&
         selection->route != D1L_MESHCORE_ROUTE_FLOOD) ||
        !d1l_meshcore_wire_path_len_valid(selection->path_len) ||
        selection->path_byte_len !=
            d1l_meshcore_wire_path_byte_len(selection->path_len) ||
        selection->path_hash_bytes !=
            d1l_meshcore_wire_path_hash_size(selection->path_len) ||
        selection->path_hops !=
            d1l_meshcore_wire_path_hash_count(selection->path_len)) {
        return false;
    }
    return selection->route != D1L_MESHCORE_ROUTE_FLOOD ||
           (selection->path_byte_len == 0U && selection->path_hops == 0U);
}

void d1l_meshcore_admin_binding_wipe(
    d1l_meshcore_admin_binding_t *binding)
{
    d1l_meshcore_admin_secure_zero(binding,
                                   binding ? sizeof(*binding) : 0U);
}

void d1l_meshcore_admin_context_wipe(
    d1l_meshcore_admin_context_t *context)
{
    d1l_meshcore_admin_secure_zero(context,
                                   context ? sizeof(*context) : 0U);
}

esp_err_t d1l_meshcore_admin_build_login_packet(
    const d1l_settings_t *settings, const d1l_contact_entry_t *contact,
    const d1l_meshcore_route_selection_t *selection, const char *password,
    uint32_t timestamp, d1l_meshcore_admin_derive_secret_fn derive_secret,
    d1l_meshcore_admin_encrypt_fn encrypt,
    d1l_meshcore_admin_binding_t *out_binding, uint8_t *raw,
    size_t raw_size, uint8_t *out_len)
{
    if (!settings || !settings->identity_ready || !contact || !selection ||
        !password || !derive_secret || !encrypt || !out_binding || !raw ||
        !out_len || !d1l_meshcore_admin_route_valid(selection)) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_meshcore_admin_binding_wipe(out_binding);
    const size_t password_len = strnlen(
        password, D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U);
    const d1l_meshcore_admin_role_t role =
        d1l_meshcore_admin_role_for_contact(contact);
    if (password_len > D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES ||
        (role == D1L_MESHCORE_ADMIN_ROLE_ROOM && password_len == 0U) ||
        (role != D1L_MESHCORE_ADMIN_ROLE_REPEATER &&
         role != D1L_MESHCORE_ADMIN_ROLE_ROOM) ||
        !hex_to_key(out_binding->peer_public_key,
                    sizeof(out_binding->peer_public_key),
                    contact->public_key_hex)) {
        d1l_meshcore_admin_binding_wipe(out_binding);
        return ESP_ERR_INVALID_STATE;
    }
    out_binding->role = role;
    snprintf(out_binding->fingerprint, sizeof(out_binding->fingerprint), "%s",
             contact->fingerprint);
    memcpy(out_binding->local_public_key, settings->identity_public_key,
           sizeof(out_binding->local_public_key));
    esp_err_t ret = derive_secret(
        out_binding->peer_public_key, out_binding->local_public_key,
        out_binding->session_secret);
    if (ret != ESP_OK) {
        d1l_meshcore_admin_binding_wipe(out_binding);
        return ret;
    }

    size_t index = 0U;
    const uint8_t header = (uint8_t)(
        (D1L_MESHCORE_ADMIN_ANON_REQUEST_TYPE << 2U) | selection->route);
    if (!d1l_meshcore_wire_write_prefix(
            header, 0U, 0U, selection->path_len,
            selection->path_byte_len > 0U ? selection->path : NULL,
            raw, raw_size, &index) ||
        raw_size - index < 1U + D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES) {
        d1l_meshcore_admin_binding_wipe(out_binding);
        return ESP_ERR_INVALID_SIZE;
    }
    raw[index++] = out_binding->peer_public_key[0];
    memcpy(&raw[index], out_binding->local_public_key,
           D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES);
    index += D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES;

    uint8_t plaintext[D1L_MESHCORE_ADMIN_MAX_LOGIN_REQUEST_BYTES] = {0};
    size_t plaintext_len = 0U;
    if (!d1l_meshcore_admin_encode_login_request(
            role, timestamp,
            role == D1L_MESHCORE_ADMIN_ROLE_ROOM ?
                timestamp : 0U,
            (const uint8_t *)password, password_len,
            plaintext, sizeof(plaintext), &plaintext_len)) {
        d1l_meshcore_admin_binding_wipe(out_binding);
        d1l_meshcore_admin_secure_zero(plaintext, sizeof(plaintext));
        return ESP_ERR_INVALID_ARG;
    }
    size_t cipher_len = 0U;
    ret = encrypt(out_binding->session_secret, &raw[index], raw_size - index,
                  plaintext, plaintext_len, &cipher_len);
    d1l_meshcore_admin_secure_zero(plaintext, sizeof(plaintext));
    if (ret != ESP_OK || index + cipher_len > UINT8_MAX) {
        d1l_meshcore_admin_binding_wipe(out_binding);
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }
    index += cipher_len;
    *out_len = (uint8_t)index;
    return ESP_OK;
}

esp_err_t d1l_meshcore_admin_build_status_packet(
    const d1l_settings_t *settings,
    const d1l_meshcore_admin_binding_t *binding,
    const d1l_meshcore_route_selection_t *selection, uint32_t tag,
    uint32_t uniqueness, d1l_meshcore_admin_encrypt_fn encrypt,
    uint8_t *raw, size_t raw_size, uint8_t *out_len)
{
    if (!settings || !settings->identity_ready || !binding || !selection ||
        tag == 0U || !encrypt || !raw || !out_len ||
        (binding->role != D1L_MESHCORE_ADMIN_ROLE_REPEATER &&
         binding->role != D1L_MESHCORE_ADMIN_ROLE_ROOM) ||
        !same_bytes(settings->identity_public_key, binding->local_public_key,
                    sizeof(binding->local_public_key)) ||
        !d1l_meshcore_admin_route_valid(selection)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t index = 0U;
    const uint8_t header = (uint8_t)(
        (D1L_MESHCORE_ADMIN_REQUEST_TYPE << 2U) | selection->route);
    if (!d1l_meshcore_wire_write_prefix(
            header, 0U, 0U, selection->path_len,
            selection->path_byte_len > 0U ? selection->path : NULL,
            raw, raw_size, &index) || raw_size - index < 2U) {
        return ESP_ERR_INVALID_SIZE;
    }
    raw[index++] = binding->peer_public_key[0];
    raw[index++] = binding->local_public_key[0];

    uint8_t plaintext[D1L_MESHCORE_ADMIN_REQUEST_BYTES] = {0};
    if (!d1l_meshcore_admin_encode_status_request(tag, uniqueness,
                                                   plaintext)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t cipher_len = 0U;
    const esp_err_t ret = encrypt(
        binding->session_secret, &raw[index], raw_size - index, plaintext,
        sizeof(plaintext), &cipher_len);
    d1l_meshcore_admin_secure_zero(plaintext, sizeof(plaintext));
    if (ret != ESP_OK || index + cipher_len > UINT8_MAX) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }
    index += cipher_len;
    *out_len = (uint8_t)index;
    return ESP_OK;
}

esp_err_t d1l_meshcore_admin_build_query_packet(
    const d1l_settings_t *settings,
    const d1l_meshcore_admin_binding_t *binding,
    const d1l_meshcore_route_selection_t *selection,
    d1l_meshcore_admin_query_t query, uint16_t offset, uint32_t tag,
    uint32_t uniqueness, d1l_meshcore_admin_encrypt_fn encrypt,
    uint8_t *raw, size_t raw_size, uint8_t *out_len)
{
    if (!settings || !settings->identity_ready || !binding || !selection ||
        tag == 0U || !encrypt || !raw || !out_len ||
        (binding->role != D1L_MESHCORE_ADMIN_ROLE_REPEATER &&
         binding->role != D1L_MESHCORE_ADMIN_ROLE_ROOM) ||
        !same_bytes(settings->identity_public_key, binding->local_public_key,
                    sizeof(binding->local_public_key)) ||
        !d1l_meshcore_admin_route_valid(selection)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t index = 0U;
    const uint8_t header = (uint8_t)(
        (D1L_MESHCORE_ADMIN_REQUEST_TYPE << 2U) | selection->route);
    if (!d1l_meshcore_wire_write_prefix(
            header, 0U, 0U, selection->path_len,
            selection->path_byte_len > 0U ? selection->path : NULL,
            raw, raw_size, &index) || raw_size - index < 2U) {
        return ESP_ERR_INVALID_SIZE;
    }
    raw[index++] = binding->peer_public_key[0];
    raw[index++] = binding->local_public_key[0];

    uint8_t plaintext[D1L_MESHCORE_ADMIN_MAX_QUERY_REQUEST_BYTES] = {0};
    size_t plaintext_len = 0U;
    if (!d1l_meshcore_admin_encode_query_request(
            query, tag, offset, uniqueness, plaintext, sizeof(plaintext),
            &plaintext_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t cipher_len = 0U;
    const esp_err_t ret = encrypt(
        binding->session_secret, &raw[index], raw_size - index, plaintext,
        plaintext_len, &cipher_len);
    d1l_meshcore_admin_secure_zero(plaintext, sizeof(plaintext));
    if (ret != ESP_OK || index + cipher_len > UINT8_MAX) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }
    index += cipher_len;
    *out_len = (uint8_t)index;
    return ESP_OK;
}

esp_err_t d1l_meshcore_admin_build_mutation_packet(
    const d1l_settings_t *settings,
    const d1l_meshcore_admin_binding_t *binding,
    const d1l_meshcore_route_selection_t *selection,
    d1l_meshcore_admin_mutation_t mutation, uint32_t timestamp,
    d1l_meshcore_admin_encrypt_fn encrypt,
    uint8_t *raw, size_t raw_size, uint8_t *out_len)
{
    const char *command = d1l_meshcore_admin_mutation_command(mutation);
    if (!settings || !settings->identity_ready || !binding || !selection ||
        !command || timestamp == 0U || !encrypt || !raw || !out_len ||
        (binding->role != D1L_MESHCORE_ADMIN_ROLE_REPEATER &&
         binding->role != D1L_MESHCORE_ADMIN_ROLE_ROOM) ||
        !same_bytes(settings->identity_public_key, binding->local_public_key,
                    sizeof(binding->local_public_key)) ||
        !d1l_meshcore_admin_route_valid(selection)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t command_len = strlen(command);
    if (command_len == 0U ||
        command_len > D1L_MESHCORE_ADMIN_MUTATION_REPLY_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t index = 0U;
    const uint8_t header =
        selection->route == D1L_MESHCORE_ROUTE_DIRECT ?
            D1L_MESHCORE_HEADER_DM_TEXT_DIRECT :
            D1L_MESHCORE_HEADER_DM_TEXT_FLOOD;
    if (!d1l_meshcore_wire_write_prefix(
            header, 0U, 0U, selection->path_len,
            selection->path_byte_len > 0U ? selection->path : NULL,
            raw, raw_size, &index) ||
        raw_size - index < 2U) {
        return ESP_ERR_INVALID_SIZE;
    }
    raw[index++] = binding->peer_public_key[0];
    raw[index++] = binding->local_public_key[0];

    uint8_t plaintext[
        5U + D1L_MESHCORE_ADMIN_MUTATION_REPLY_MAX_BYTES] = {0};
    write_le32(plaintext, timestamp);
    plaintext[4] = (uint8_t)(D1L_MESHCORE_TXT_TYPE_CLI_DATA << 2U);
    memcpy(&plaintext[5], command, command_len);
    size_t cipher_len = 0U;
    const esp_err_t ret = encrypt(
        binding->session_secret, &raw[index], raw_size - index, plaintext,
        5U + command_len, &cipher_len);
    d1l_meshcore_admin_secure_zero(plaintext, sizeof(plaintext));
    if (ret != ESP_OK || index + cipher_len > UINT8_MAX) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }
    index += cipher_len;
    *out_len = (uint8_t)index;
    return ESP_OK;
}

esp_err_t d1l_meshcore_admin_build_cli_packet(
    const d1l_settings_t *settings,
    const d1l_meshcore_admin_binding_t *binding,
    const d1l_meshcore_route_selection_t *selection,
    const char *command, uint32_t timestamp,
    d1l_meshcore_admin_encrypt_fn encrypt,
    uint8_t *raw, size_t raw_size, uint8_t *out_len)
{
    if (!settings || !settings->identity_ready || !binding || !selection ||
        !d1l_meshcore_admin_cli_command_valid(command) ||
        timestamp == 0U || !encrypt || !raw || !out_len ||
        (binding->role != D1L_MESHCORE_ADMIN_ROLE_REPEATER &&
         binding->role != D1L_MESHCORE_ADMIN_ROLE_ROOM) ||
        !same_bytes(settings->identity_public_key, binding->local_public_key,
                    sizeof(binding->local_public_key)) ||
        !d1l_meshcore_admin_route_valid(selection)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t command_len = strlen(command);
    size_t index = 0U;
    const uint8_t header =
        selection->route == D1L_MESHCORE_ROUTE_DIRECT ?
            D1L_MESHCORE_HEADER_DM_TEXT_DIRECT :
            D1L_MESHCORE_HEADER_DM_TEXT_FLOOD;
    if (!d1l_meshcore_wire_write_prefix(
            header, 0U, 0U, selection->path_len,
            selection->path_byte_len > 0U ? selection->path : NULL,
            raw, raw_size, &index) ||
        raw_size - index < 2U) {
        return ESP_ERR_INVALID_SIZE;
    }
    raw[index++] = binding->peer_public_key[0];
    raw[index++] = binding->local_public_key[0];

    uint8_t plaintext[
        5U + D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES] = {0};
    write_le32(plaintext, timestamp);
    plaintext[4] = (uint8_t)(D1L_MESHCORE_TXT_TYPE_CLI_DATA << 2U);
    memcpy(&plaintext[5], command, command_len);
    size_t cipher_len = 0U;
    const esp_err_t ret = encrypt(
        binding->session_secret, &raw[index], raw_size - index, plaintext,
        5U + command_len, &cipher_len);
    d1l_meshcore_admin_secure_zero(plaintext, sizeof(plaintext));
    if (ret != ESP_OK || index + cipher_len > UINT8_MAX) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }
    index += cipher_len;
    *out_len = (uint8_t)index;
    return ESP_OK;
}

static void clear_session_locked(void)
{
    d1l_meshcore_admin_reset(&s_session);
    s_fingerprint[0] = '\0';
}

static d1l_meshcore_admin_state_t failure_state_for_error(esp_err_t reason)
{
    if (reason == ESP_ERR_TIMEOUT) {
        return D1L_MESHCORE_ADMIN_RADIO_BUSY;
    }
    if (reason == ESP_ERR_NOT_SUPPORTED) {
        return D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL;
    }
    if (reason == ESP_ERR_NOT_ALLOWED) {
        return D1L_MESHCORE_ADMIN_REJECTED_CREDENTIALS;
    }
    return D1L_MESHCORE_ADMIN_DISCONNECTED;
}

static void fail_session_locked(
    d1l_meshcore_admin_state_t failure_state, esp_err_t reason)
{
    (void)d1l_meshcore_admin_fail(&s_session, failure_state);
    s_metrics.last_error = reason;
}

static bool copy_session_context_locked(
    d1l_meshcore_admin_context_t *out_context)
{
    if (!out_context || s_fingerprint[0] == '\0') {
        return false;
    }
    memset(out_context, 0, sizeof(*out_context));
    snprintf(out_context->binding.fingerprint,
             sizeof(out_context->binding.fingerprint), "%s", s_fingerprint);
    out_context->binding.role = s_session.role;
    memcpy(out_context->binding.peer_public_key, s_session.peer_public_key,
           sizeof(out_context->binding.peer_public_key));
    memcpy(out_context->binding.local_public_key, s_session.local_public_key,
           sizeof(out_context->binding.local_public_key));
    memcpy(out_context->binding.session_secret, s_session.session_secret,
           sizeof(out_context->binding.session_secret));
    out_context->state = s_session.state;
    out_context->generation = s_session.generation;
    out_context->permissions = s_session.permissions;
    out_context->request_deadline_us = s_session.request_deadline_us;
    out_context->pending_mutation = s_session.pending_mutation;
    out_context->pending_query = s_session.pending_query;
    out_context->pending_query_offset = s_session.pending_query_offset;
    out_context->pending_cli_sensitive = s_session.pending_cli_sensitive;
    out_context->pending_cli_read_only = s_session.pending_cli_read_only;
    out_context->pending_cli_reply_profile =
        s_session.pending_cli_reply_profile;
    return true;
}

static bool copy_context_locked(d1l_meshcore_admin_context_t *out_context,
                                bool authenticated)
{
    const bool eligible = authenticated
        ? s_session.state == D1L_MESHCORE_ADMIN_AUTHENTICATED
        : (s_session.state == D1L_MESHCORE_ADMIN_LOGIN_PENDING ||
           s_session.state == D1L_MESHCORE_ADMIN_STATUS_PENDING ||
           s_session.state == D1L_MESHCORE_ADMIN_QUERY_PENDING);
    return eligible && copy_session_context_locked(out_context);
}

static bool admin_session_active_locked(void)
{
    return s_session.state == D1L_MESHCORE_ADMIN_AUTHENTICATED ||
           s_session.state == D1L_MESHCORE_ADMIN_STATUS_PENDING ||
           s_session.state == D1L_MESHCORE_ADMIN_MUTATION_PENDING ||
           s_session.state == D1L_MESHCORE_ADMIN_CLI_PENDING ||
           s_session.state == D1L_MESHCORE_ADMIN_QUERY_PENDING;
}

static bool binding_matches_locked(
    const d1l_meshcore_admin_binding_t *binding)
{
    return binding &&
           strncmp(s_fingerprint, binding->fingerprint,
                   sizeof(s_fingerprint)) == 0 &&
           d1l_meshcore_admin_binding_matches(
               &s_session, binding->role, binding->peer_public_key,
               binding->local_public_key, binding->session_secret);
}

void d1l_meshcore_admin_runtime_init(void)
{
    d1l_store_lock_take(&s_lock);
    d1l_meshcore_admin_secure_zero(&s_session, sizeof(s_session));
    d1l_meshcore_admin_reset(&s_session);
    d1l_meshcore_admin_replay_cache_clear(&s_replay_cache);
    s_fingerprint[0] = '\0';
    memset(&s_metrics, 0, sizeof(s_metrics));
    s_metrics.last_error = ESP_OK;
    d1l_store_lock_give(&s_lock);
}

bool d1l_meshcore_admin_runtime_begin_login(
    const d1l_meshcore_admin_binding_t *binding, uint64_t now_us,
    uint32_t *out_generation)
{
    if (!binding || !out_generation || binding->fingerprint[0] == '\0') {
        return false;
    }
    d1l_store_lock_take(&s_lock);
    const bool began = d1l_meshcore_admin_begin_login(
        &s_session, binding->role, binding->peer_public_key,
        binding->local_public_key, binding->session_secret,
        deadline_after(now_us, D1L_MESHCORE_ADMIN_LOGIN_TIMEOUT_US),
        D1L_MESHCORE_ADMIN_IDLE_TIMEOUT_US,
        D1L_MESHCORE_ADMIN_ABSOLUTE_TIMEOUT_US);
    if (began) {
        snprintf(s_fingerprint, sizeof(s_fingerprint), "%s",
                 binding->fingerprint);
        *out_generation = s_session.generation;
        s_metrics.last_error = ESP_OK;
    }
    d1l_store_lock_give(&s_lock);
    return began;
}

void d1l_meshcore_admin_runtime_note_login_tx(uint32_t generation,
                                              esp_err_t result)
{
    d1l_store_lock_take(&s_lock);
    if (result == ESP_OK) {
        s_metrics.login_tx_queued++;
    } else {
        if (s_session.generation == generation &&
            s_session.state == D1L_MESHCORE_ADMIN_LOGIN_PENDING) {
            fail_session_locked(failure_state_for_error(result), result);
        }
        s_metrics.last_error = result;
    }
    d1l_store_lock_give(&s_lock);
}

bool d1l_meshcore_admin_runtime_capture_pending(
    d1l_meshcore_admin_context_t *out_context)
{
    d1l_store_lock_take(&s_lock);
    const bool copied = copy_context_locked(out_context, false);
    d1l_store_lock_give(&s_lock);
    return copied;
}

bool d1l_meshcore_admin_runtime_capture_authenticated(
    d1l_meshcore_admin_context_t *out_context)
{
    d1l_store_lock_take(&s_lock);
    const bool copied = copy_context_locked(out_context, true);
    d1l_store_lock_give(&s_lock);
    return copied;
}

bool d1l_meshcore_admin_runtime_capture_active(
    d1l_meshcore_admin_context_t *out_context)
{
    d1l_store_lock_take(&s_lock);
    const bool copied = admin_session_active_locked() &&
                        copy_session_context_locked(out_context);
    d1l_store_lock_give(&s_lock);
    return copied;
}

bool d1l_meshcore_admin_runtime_capture_mutation_pending(
    d1l_meshcore_admin_context_t *out_context)
{
    if (!out_context) {
        return false;
    }
    d1l_store_lock_take(&s_lock);
    const bool eligible =
        s_session.state == D1L_MESHCORE_ADMIN_MUTATION_PENDING &&
        s_fingerprint[0] != '\0';
    if (eligible) {
        (void)copy_session_context_locked(out_context);
    }
    d1l_store_lock_give(&s_lock);
    return eligible;
}

bool d1l_meshcore_admin_runtime_capture_cli_pending(
    d1l_meshcore_admin_context_t *out_context)
{
    if (!out_context) {
        return false;
    }
    d1l_store_lock_take(&s_lock);
    const bool eligible =
        s_session.state == D1L_MESHCORE_ADMIN_CLI_PENDING &&
        s_fingerprint[0] != '\0';
    if (eligible) {
        (void)copy_session_context_locked(out_context);
    }
    d1l_store_lock_give(&s_lock);
    return eligible;
}

bool d1l_meshcore_admin_runtime_validate_binding(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation)
{
    d1l_store_lock_take(&s_lock);
    const bool valid = s_session.generation == generation &&
                       binding_matches_locked(binding);
    if (!valid && s_session.generation == generation) {
        fail_session_locked(
            D1L_MESHCORE_ADMIN_DISCONNECTED, ESP_ERR_INVALID_STATE);
    }
    d1l_store_lock_give(&s_lock);
    return valid;
}

bool d1l_meshcore_admin_runtime_note_room_activity(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    uint64_t now_us)
{
    d1l_store_lock_take(&s_lock);
    bool valid = admin_session_active_locked() &&
                 s_session.role == D1L_MESHCORE_ADMIN_ROLE_ROOM &&
                 s_session.generation == generation &&
                 binding_matches_locked(binding);
    if (valid &&
        !d1l_meshcore_admin_note_authenticated_activity(
            &s_session, now_us)) {
        s_metrics.response_expired++;
        s_metrics.last_error = ESP_ERR_TIMEOUT;
        valid = false;
    }
    d1l_store_lock_give(&s_lock);
    return valid;
}

bool d1l_meshcore_admin_runtime_begin_status(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    uint32_t tag, uint64_t now_us, uint32_t *out_request_generation)
{
    if (!out_request_generation) {
        return false;
    }
    d1l_store_lock_take(&s_lock);
    const bool valid = s_session.generation == generation &&
                       binding_matches_locked(binding);
    bool began = false;
    if (valid) {
        began = d1l_meshcore_admin_begin_status_request(
            &s_session, tag, now_us,
            deadline_after(now_us, D1L_MESHCORE_ADMIN_REQUEST_TIMEOUT_US));
    } else if (s_session.generation == generation) {
        fail_session_locked(
            D1L_MESHCORE_ADMIN_DISCONNECTED, ESP_ERR_INVALID_STATE);
    }
    if (began) {
        *out_request_generation = s_session.generation;
        s_metrics.last_error = ESP_OK;
    } else if (s_session.state == D1L_MESHCORE_ADMIN_TIMED_OUT) {
        s_metrics.response_expired++;
        s_metrics.last_error = ESP_ERR_TIMEOUT;
    }
    d1l_store_lock_give(&s_lock);
    return began;
}

void d1l_meshcore_admin_runtime_note_status_tx(uint32_t request_generation,
                                               uint32_t tag,
                                               esp_err_t result)
{
    (void)tag;
    d1l_store_lock_take(&s_lock);
    if (result == ESP_OK) {
        s_metrics.status_tx_queued++;
    } else {
        if (s_session.generation == request_generation) {
            fail_session_locked(failure_state_for_error(result), result);
        }
        s_metrics.last_error = result;
    }
    d1l_store_lock_give(&s_lock);
}

bool d1l_meshcore_admin_runtime_begin_query(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    d1l_meshcore_admin_query_t query, uint16_t offset, uint32_t tag,
    uint64_t now_us, uint32_t *out_request_generation)
{
    if (!out_request_generation) {
        return false;
    }
    d1l_store_lock_take(&s_lock);
    const bool valid = s_session.generation == generation &&
                       binding_matches_locked(binding);
    bool began = false;
    if (valid) {
        began = d1l_meshcore_admin_begin_query_request(
            &s_session, query, offset, tag, now_us,
            deadline_after(now_us, D1L_MESHCORE_ADMIN_REQUEST_TIMEOUT_US));
    } else if (s_session.generation == generation) {
        fail_session_locked(
            D1L_MESHCORE_ADMIN_DISCONNECTED, ESP_ERR_INVALID_STATE);
    }
    if (began) {
        *out_request_generation = s_session.generation;
        s_metrics.last_error = ESP_OK;
    } else if (s_session.state == D1L_MESHCORE_ADMIN_TIMED_OUT) {
        s_metrics.response_expired++;
        s_metrics.last_error = ESP_ERR_TIMEOUT;
    }
    d1l_store_lock_give(&s_lock);
    return began;
}

void d1l_meshcore_admin_runtime_note_query_tx(
    uint32_t request_generation, d1l_meshcore_admin_query_t query,
    uint32_t tag, esp_err_t result)
{
    (void)query;
    (void)tag;
    d1l_store_lock_take(&s_lock);
    if (result == ESP_OK) {
        s_metrics.query_tx_queued++;
    } else {
        if (s_session.generation == request_generation) {
            fail_session_locked(failure_state_for_error(result), result);
        }
        s_metrics.last_error = result;
    }
    d1l_store_lock_give(&s_lock);
}

bool d1l_meshcore_admin_runtime_begin_mutation(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    d1l_meshcore_admin_mutation_t mutation, uint32_t tag, uint64_t now_us,
    uint32_t *out_request_generation)
{
    if (!out_request_generation) {
        return false;
    }
    d1l_store_lock_take(&s_lock);
    const bool valid = s_session.generation == generation &&
                       binding_matches_locked(binding);
    bool began = false;
    if (valid) {
        began = d1l_meshcore_admin_begin_mutation(
            &s_session, mutation, tag, now_us,
            deadline_after(now_us, D1L_MESHCORE_ADMIN_REQUEST_TIMEOUT_US));
    } else if (s_session.generation == generation) {
        fail_session_locked(
            D1L_MESHCORE_ADMIN_DISCONNECTED, ESP_ERR_INVALID_STATE);
    }
    if (began) {
        *out_request_generation = s_session.generation;
        s_metrics.last_error = ESP_OK;
    } else if (s_session.state == D1L_MESHCORE_ADMIN_TIMED_OUT) {
        s_metrics.response_expired++;
        s_metrics.last_error = ESP_ERR_TIMEOUT;
    }
    d1l_store_lock_give(&s_lock);
    return began;
}

void d1l_meshcore_admin_runtime_note_mutation_tx(
    uint32_t request_generation, d1l_meshcore_admin_mutation_t mutation,
    uint32_t tag, esp_err_t result)
{
    (void)mutation;
    (void)tag;
    d1l_store_lock_take(&s_lock);
    if (result == ESP_OK) {
        s_metrics.mutation_tx_queued++;
    } else {
        if (s_session.generation == request_generation) {
            fail_session_locked(failure_state_for_error(result), result);
        }
        s_metrics.last_error = result;
    }
    d1l_store_lock_give(&s_lock);
}

bool d1l_meshcore_admin_runtime_begin_cli(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    uint32_t tag, bool sensitive, bool read_only,
    d1l_meshcore_admin_cli_reply_profile_t reply_profile, uint64_t now_us,
    uint32_t *out_request_generation)
{
    if (!out_request_generation) {
        return false;
    }
    d1l_store_lock_take(&s_lock);
    const bool valid = s_session.generation == generation &&
                       binding_matches_locked(binding);
    bool began = false;
    if (valid) {
        began = d1l_meshcore_admin_begin_cli_command(
            &s_session, tag, sensitive, read_only, reply_profile, now_us,
            deadline_after(now_us, D1L_MESHCORE_ADMIN_REQUEST_TIMEOUT_US));
    } else if (s_session.generation == generation) {
        fail_session_locked(
            D1L_MESHCORE_ADMIN_DISCONNECTED, ESP_ERR_INVALID_STATE);
    }
    if (began) {
        *out_request_generation = s_session.generation;
        s_metrics.last_error = ESP_OK;
    } else if (s_session.state == D1L_MESHCORE_ADMIN_TIMED_OUT) {
        s_metrics.response_expired++;
        s_metrics.last_error = ESP_ERR_TIMEOUT;
    }
    d1l_store_lock_give(&s_lock);
    return began;
}

void d1l_meshcore_admin_runtime_note_cli_tx(
    uint32_t request_generation, uint32_t tag, esp_err_t result)
{
    (void)tag;
    d1l_store_lock_take(&s_lock);
    if (result == ESP_OK) {
        s_metrics.cli_tx_queued++;
    } else {
        if (s_session.generation == request_generation) {
            fail_session_locked(failure_state_for_error(result), result);
        }
        s_metrics.last_error = result;
    }
    d1l_store_lock_give(&s_lock);
}

static void note_response_locked(
    d1l_meshcore_admin_response_result_t result)
{
    switch (result) {
    case D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED:
        s_metrics.response_accepted++;
        s_metrics.last_error = ESP_OK;
        break;
    case D1L_MESHCORE_ADMIN_RESPONSE_REJECTED:
        s_metrics.last_error = ESP_ERR_INVALID_RESPONSE;
        break;
    case D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED:
        s_metrics.response_unmatched++;
        break;
    case D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED:
        s_metrics.response_malformed++;
        s_metrics.last_error = ESP_ERR_INVALID_RESPONSE;
        break;
    case D1L_MESHCORE_ADMIN_RESPONSE_EXPIRED:
        s_metrics.response_expired++;
        s_metrics.last_error = ESP_ERR_TIMEOUT;
        break;
    case D1L_MESHCORE_ADMIN_RESPONSE_REPLAYED:
        s_metrics.response_replayed++;
        s_metrics.last_error = ESP_ERR_INVALID_RESPONSE;
        break;
    default:
        break;
    }
}

d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_runtime_dispatch_mutation_response(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    uint32_t response_timestamp, const uint8_t *text, size_t text_len,
    uint64_t now_us, bool *out_considered)
{
    if (out_considered) {
        *out_considered = false;
    }
    if (!binding || !text) {
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    d1l_store_lock_take(&s_lock);
    const bool same_attempt =
        s_session.state == D1L_MESHCORE_ADMIN_MUTATION_PENDING &&
        s_session.generation == generation &&
        strncmp(s_fingerprint, binding->fingerprint,
                sizeof(s_fingerprint)) == 0;
    if (!same_attempt) {
        d1l_store_lock_give(&s_lock);
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    if (out_considered) {
        *out_considered = true;
    }
    if (!binding_matches_locked(binding)) {
        fail_session_locked(
            D1L_MESHCORE_ADMIN_DISCONNECTED, ESP_ERR_INVALID_STATE);
        d1l_store_lock_give(&s_lock);
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    const d1l_meshcore_admin_response_result_t result =
        d1l_meshcore_admin_accept_mutation_response(
            &s_session, binding->peer_public_key, response_timestamp,
            text, text_len, now_us);
    if (result == D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED &&
        s_session.state != D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL) {
        fail_session_locked(
            D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL,
            ESP_ERR_INVALID_RESPONSE);
    }
    if (result == D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED) {
        s_metrics.mutation_accepted++;
    } else if (result == D1L_MESHCORE_ADMIN_RESPONSE_REJECTED) {
        s_metrics.mutation_rejected++;
    }
    note_response_locked(result);
    d1l_store_lock_give(&s_lock);
    return result;
}

d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_runtime_dispatch_cli_response(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    uint32_t response_timestamp, const uint8_t *text, size_t text_len,
    uint64_t now_us, bool *out_considered)
{
    if (out_considered) {
        *out_considered = false;
    }
    if (!binding || !text) {
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    d1l_store_lock_take(&s_lock);
    const bool same_attempt =
        s_session.state == D1L_MESHCORE_ADMIN_CLI_PENDING &&
        s_session.generation == generation &&
        strncmp(s_fingerprint, binding->fingerprint,
                sizeof(s_fingerprint)) == 0;
    if (!same_attempt) {
        d1l_store_lock_give(&s_lock);
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    if (out_considered) {
        *out_considered = true;
    }
    if (!binding_matches_locked(binding)) {
        fail_session_locked(
            D1L_MESHCORE_ADMIN_DISCONNECTED, ESP_ERR_INVALID_STATE);
        d1l_store_lock_give(&s_lock);
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    const d1l_meshcore_admin_response_result_t result =
        d1l_meshcore_admin_accept_cli_response(
            &s_session, binding->peer_public_key, response_timestamp,
            text, text_len, now_us);
    if (result == D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED &&
        s_session.state != D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL) {
        fail_session_locked(
            D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL,
            ESP_ERR_INVALID_RESPONSE);
    }
    if (result == D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED) {
        s_metrics.cli_accepted++;
    } else if (result == D1L_MESHCORE_ADMIN_RESPONSE_REJECTED) {
        s_metrics.cli_rejected++;
    }
    note_response_locked(result);
    d1l_store_lock_give(&s_lock);
    return result;
}

d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_runtime_dispatch_response(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    const uint8_t *plaintext, size_t plaintext_len, uint64_t now_us,
    bool *out_considered)
{
    if (out_considered) {
        *out_considered = false;
    }
    if (!binding || !plaintext) {
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    d1l_store_lock_take(&s_lock);
    const bool pending =
        s_session.state == D1L_MESHCORE_ADMIN_LOGIN_PENDING ||
        s_session.state == D1L_MESHCORE_ADMIN_STATUS_PENDING ||
        s_session.state == D1L_MESHCORE_ADMIN_QUERY_PENDING;
    const bool query_attempt =
        s_session.state == D1L_MESHCORE_ADMIN_QUERY_PENDING;
    const bool same_attempt = pending && s_session.generation == generation &&
        strncmp(s_fingerprint, binding->fingerprint,
                sizeof(s_fingerprint)) == 0;
    if (!same_attempt) {
        d1l_store_lock_give(&s_lock);
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }
    if (out_considered) {
        *out_considered = true;
    }
    if (!binding_matches_locked(binding)) {
        fail_session_locked(
            D1L_MESHCORE_ADMIN_DISCONNECTED, ESP_ERR_INVALID_STATE);
        d1l_store_lock_give(&s_lock);
        return D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED;
    }

    d1l_meshcore_admin_response_result_t result;
    esp_err_t durable_failure = ESP_OK;
    if (s_session.state == D1L_MESHCORE_ADMIN_LOGIN_PENDING) {
        d1l_meshcore_admin_session_t candidate_session = s_session;
        d1l_meshcore_admin_replay_cache_t candidate_replay = s_replay_cache;
        result = d1l_meshcore_admin_accept_login_response(
            &candidate_session, &candidate_replay,
            binding->peer_public_key, plaintext, plaintext_len, now_us);
        if (result == D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED) {
            const esp_err_t durable_ret = durable_replay_commit(
                binding->peer_public_key, plaintext,
                candidate_session.server_timestamp);
            if (durable_ret == ESP_OK) {
                s_session = candidate_session;
                s_replay_cache = candidate_replay;
            } else if (durable_ret == ESP_ERR_INVALID_RESPONSE) {
                fail_session_locked(
                    D1L_MESHCORE_ADMIN_TIMED_OUT,
                    ESP_ERR_INVALID_RESPONSE);
                result = D1L_MESHCORE_ADMIN_RESPONSE_REPLAYED;
            } else {
                fail_session_locked(
                    D1L_MESHCORE_ADMIN_DISCONNECTED, durable_ret);
                durable_failure = durable_ret;
                result = D1L_MESHCORE_ADMIN_RESPONSE_REJECTED;
            }
        } else {
            s_session = candidate_session;
            s_replay_cache = candidate_replay;
        }
        d1l_meshcore_admin_secure_zero(
            &candidate_session, sizeof(candidate_session));
        d1l_meshcore_admin_secure_zero(
            &candidate_replay, sizeof(candidate_replay));
    } else if (s_session.state == D1L_MESHCORE_ADMIN_STATUS_PENDING) {
        result = d1l_meshcore_admin_accept_status_response(
            &s_session, binding->peer_public_key, plaintext, plaintext_len,
            now_us);
    } else {
        result = d1l_meshcore_admin_accept_query_response(
            &s_session, binding->peer_public_key, plaintext, plaintext_len,
            now_us);
    }
    if (result == D1L_MESHCORE_ADMIN_RESPONSE_MALFORMED &&
        s_session.state != D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL) {
        fail_session_locked(
            D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL,
            ESP_ERR_INVALID_RESPONSE);
    }
    if (query_attempt) {
        if (result == D1L_MESHCORE_ADMIN_RESPONSE_ACCEPTED) {
            s_metrics.query_accepted++;
        } else if (result != D1L_MESHCORE_ADMIN_RESPONSE_UNMATCHED) {
            s_metrics.query_rejected++;
        }
    }
    note_response_locked(result);
    if (durable_failure != ESP_OK) {
        s_metrics.last_error = durable_failure;
    }
    d1l_store_lock_give(&s_lock);
    return result;
}

bool d1l_meshcore_admin_runtime_expire(uint64_t now_us)
{
    d1l_store_lock_take(&s_lock);
    const bool expired = d1l_meshcore_admin_expire_if_due(&s_session, now_us);
    if (expired) {
        s_metrics.response_expired++;
        s_metrics.last_error = ESP_ERR_TIMEOUT;
    }
    d1l_store_lock_give(&s_lock);
    return expired;
}

void d1l_meshcore_admin_runtime_snapshot(
    d1l_meshcore_admin_runtime_snapshot_t *out_snapshot)
{
    if (!out_snapshot) {
        return;
    }
    d1l_meshcore_admin_runtime_snapshot_t snapshot = {0};
    d1l_store_lock_take(&s_lock);
    snapshot.state = s_session.state;
    snapshot.role = s_session.role;
    snapshot.generation = s_session.generation;
    snprintf(snapshot.fingerprint, sizeof(snapshot.fingerprint), "%s",
             s_fingerprint);
    snapshot.permissions = s_session.permissions;
    snapshot.firmware_level = s_session.firmware_level;
    snapshot.server_timestamp = s_session.server_timestamp;
    snapshot.pending_tag = s_session.pending_tag;
    snapshot.pending_mutation = s_session.pending_mutation;
    snapshot.last_mutation = s_session.last_mutation;
    snapshot.pending_query = s_session.pending_query;
    snapshot.pending_query_offset = s_session.pending_query_offset;
    snapshot.last_mutation_success = s_session.last_mutation_success;
    snapshot.cli_reply_valid = s_session.cli_reply_valid;
    snapshot.cli_reply_redacted = s_session.cli_reply_redacted;
    snapshot.cli_reply_success = s_session.cli_reply_success;
    memcpy(snapshot.cli_reply, s_session.cli_reply,
           sizeof(snapshot.cli_reply));
    snapshot.status_valid = s_session.status_valid;
    snapshot.status = s_session.status;
    snapshot.query_result = s_session.query_result;
    snapshot.login_tx_queued = s_metrics.login_tx_queued;
    snapshot.status_tx_queued = s_metrics.status_tx_queued;
    snapshot.query_tx_queued = s_metrics.query_tx_queued;
    snapshot.query_accepted = s_metrics.query_accepted;
    snapshot.query_rejected = s_metrics.query_rejected;
    snapshot.mutation_tx_queued = s_metrics.mutation_tx_queued;
    snapshot.mutation_accepted = s_metrics.mutation_accepted;
    snapshot.mutation_rejected = s_metrics.mutation_rejected;
    snapshot.cli_tx_queued = s_metrics.cli_tx_queued;
    snapshot.cli_accepted = s_metrics.cli_accepted;
    snapshot.cli_rejected = s_metrics.cli_rejected;
    snapshot.response_accepted = s_metrics.response_accepted;
    snapshot.response_unmatched = s_metrics.response_unmatched;
    snapshot.response_malformed = s_metrics.response_malformed;
    snapshot.response_expired = s_metrics.response_expired;
    snapshot.response_replayed = s_metrics.response_replayed;
    snapshot.last_error = s_metrics.last_error;
    d1l_store_lock_give(&s_lock);
    *out_snapshot = snapshot;
}

void d1l_meshcore_admin_runtime_report_failure(esp_err_t reason)
{
    d1l_store_lock_take(&s_lock);
    const d1l_meshcore_admin_state_t failure_state =
        failure_state_for_error(reason);
    if (s_session.state != failure_state ||
        s_metrics.last_error != reason) {
        fail_session_locked(failure_state, reason);
    }
    d1l_store_lock_give(&s_lock);
}

void d1l_meshcore_admin_runtime_invalidate(esp_err_t reason)
{
    if (reason != ESP_OK) {
        d1l_meshcore_admin_runtime_report_failure(reason);
        return;
    }
    d1l_store_lock_take(&s_lock);
    clear_session_locked();
    s_metrics.last_error = ESP_OK;
    d1l_store_lock_give(&s_lock);
}

void d1l_meshcore_admin_runtime_logout(void)
{
    d1l_meshcore_admin_runtime_invalidate(ESP_OK);
}
