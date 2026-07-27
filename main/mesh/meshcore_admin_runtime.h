#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/settings_model.h"
#include "esp_err.h"
#include "mesh/contact_store.h"
#include "mesh/meshcore_admin_dispatch.h"
#include "mesh/meshcore_route_selection.h"

#ifdef __cplusplus
extern "C" {
#endif

#define D1L_MESHCORE_ADMIN_LOGIN_TIMEOUT_US UINT64_C(60000000)
#define D1L_MESHCORE_ADMIN_REQUEST_TIMEOUT_US UINT64_C(60000000)
#define D1L_MESHCORE_ADMIN_IDLE_TIMEOUT_US UINT64_C(300000000)
#define D1L_MESHCORE_ADMIN_ABSOLUTE_TIMEOUT_US UINT64_C(1800000000)

typedef struct {
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    d1l_meshcore_admin_role_t role;
    uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES];
    uint8_t local_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES];
    uint8_t session_secret[D1L_MESHCORE_ADMIN_SECRET_BYTES];
} d1l_meshcore_admin_binding_t;

typedef struct {
    d1l_meshcore_admin_binding_t binding;
    d1l_meshcore_admin_state_t state;
    uint32_t generation;
    uint8_t permissions;
    uint64_t request_deadline_us;
    d1l_meshcore_admin_mutation_t pending_mutation;
    d1l_meshcore_admin_query_t pending_query;
    uint16_t pending_query_offset;
    bool pending_cli_sensitive;
    bool pending_cli_read_only;
    d1l_meshcore_admin_cli_reply_profile_t pending_cli_reply_profile;
} d1l_meshcore_admin_context_t;

typedef struct {
    d1l_meshcore_admin_state_t state;
    d1l_meshcore_admin_role_t role;
    uint32_t generation;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    uint8_t permissions;
    uint8_t firmware_level;
    uint32_t server_timestamp;
    uint32_t pending_tag;
    d1l_meshcore_admin_mutation_t pending_mutation;
    d1l_meshcore_admin_mutation_t last_mutation;
    d1l_meshcore_admin_query_t pending_query;
    uint16_t pending_query_offset;
    bool last_mutation_success;
    bool cli_reply_valid;
    bool cli_reply_redacted;
    bool cli_reply_success;
    char cli_reply[D1L_MESHCORE_ADMIN_MAX_CLI_REPLY_BYTES + 1U];
    bool status_valid;
    d1l_meshcore_admin_status_t status;
    d1l_meshcore_admin_query_result_t query_result;
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
} d1l_meshcore_admin_runtime_snapshot_t;

typedef esp_err_t (*d1l_meshcore_admin_derive_secret_fn)(
    const uint8_t peer_public_key[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    const uint8_t expected_local_public_key[
        D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES],
    uint8_t out_secret[D1L_MESHCORE_ADMIN_SECRET_BYTES]);

typedef esp_err_t (*d1l_meshcore_admin_encrypt_fn)(
    const uint8_t secret[D1L_MESHCORE_ADMIN_SECRET_BYTES], uint8_t *dest,
    size_t dest_size, const uint8_t *src, size_t src_len,
    size_t *out_len);

d1l_meshcore_admin_role_t d1l_meshcore_admin_role_for_contact(
    const d1l_contact_entry_t *contact);
bool d1l_meshcore_admin_route_valid(
    const d1l_meshcore_route_selection_t *selection);

esp_err_t d1l_meshcore_admin_build_login_packet(
    const d1l_settings_t *settings, const d1l_contact_entry_t *contact,
    const d1l_meshcore_route_selection_t *selection, const char *password,
    uint32_t timestamp, d1l_meshcore_admin_derive_secret_fn derive_secret,
    d1l_meshcore_admin_encrypt_fn encrypt,
    d1l_meshcore_admin_binding_t *out_binding, uint8_t *raw,
    size_t raw_size, uint8_t *out_len);
esp_err_t d1l_meshcore_admin_build_status_packet(
    const d1l_settings_t *settings,
    const d1l_meshcore_admin_binding_t *binding,
    const d1l_meshcore_route_selection_t *selection, uint32_t tag,
    uint32_t uniqueness, d1l_meshcore_admin_encrypt_fn encrypt,
    uint8_t *raw, size_t raw_size, uint8_t *out_len);
esp_err_t d1l_meshcore_admin_build_query_packet(
    const d1l_settings_t *settings,
    const d1l_meshcore_admin_binding_t *binding,
    const d1l_meshcore_route_selection_t *selection,
    d1l_meshcore_admin_query_t query, uint16_t offset, uint32_t tag,
    uint32_t uniqueness, d1l_meshcore_admin_encrypt_fn encrypt,
    uint8_t *raw, size_t raw_size, uint8_t *out_len);
esp_err_t d1l_meshcore_admin_build_mutation_packet(
    const d1l_settings_t *settings,
    const d1l_meshcore_admin_binding_t *binding,
    const d1l_meshcore_route_selection_t *selection,
    d1l_meshcore_admin_mutation_t mutation, uint32_t timestamp,
    d1l_meshcore_admin_encrypt_fn encrypt,
    uint8_t *raw, size_t raw_size, uint8_t *out_len);
esp_err_t d1l_meshcore_admin_build_cli_packet(
    const d1l_settings_t *settings,
    const d1l_meshcore_admin_binding_t *binding,
    const d1l_meshcore_route_selection_t *selection,
    const char *command, uint32_t timestamp,
    d1l_meshcore_admin_encrypt_fn encrypt,
    uint8_t *raw, size_t raw_size, uint8_t *out_len);

void d1l_meshcore_admin_runtime_init(void);
void d1l_meshcore_admin_binding_wipe(
    d1l_meshcore_admin_binding_t *binding);
void d1l_meshcore_admin_context_wipe(
    d1l_meshcore_admin_context_t *context);
bool d1l_meshcore_admin_runtime_begin_login(
    const d1l_meshcore_admin_binding_t *binding, uint64_t now_us,
    uint32_t *out_generation);
void d1l_meshcore_admin_runtime_note_login_tx(uint32_t generation,
                                              esp_err_t result);
bool d1l_meshcore_admin_runtime_capture_pending(
    d1l_meshcore_admin_context_t *out_context);
bool d1l_meshcore_admin_runtime_capture_authenticated(
    d1l_meshcore_admin_context_t *out_context);
bool d1l_meshcore_admin_runtime_capture_active(
    d1l_meshcore_admin_context_t *out_context);
bool d1l_meshcore_admin_runtime_capture_mutation_pending(
    d1l_meshcore_admin_context_t *out_context);
bool d1l_meshcore_admin_runtime_capture_cli_pending(
    d1l_meshcore_admin_context_t *out_context);
bool d1l_meshcore_admin_runtime_validate_binding(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation);
bool d1l_meshcore_admin_runtime_note_room_activity(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    uint64_t now_us);
bool d1l_meshcore_admin_runtime_begin_status(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    uint32_t tag, uint64_t now_us, uint32_t *out_request_generation);
void d1l_meshcore_admin_runtime_note_status_tx(uint32_t request_generation,
                                               uint32_t tag,
                                               esp_err_t result);
bool d1l_meshcore_admin_runtime_begin_query(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    d1l_meshcore_admin_query_t query, uint16_t offset, uint32_t tag,
    uint64_t now_us, uint32_t *out_request_generation);
void d1l_meshcore_admin_runtime_note_query_tx(
    uint32_t request_generation, d1l_meshcore_admin_query_t query,
    uint32_t tag, esp_err_t result);
bool d1l_meshcore_admin_runtime_begin_mutation(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    d1l_meshcore_admin_mutation_t mutation, uint32_t tag, uint64_t now_us,
    uint32_t *out_request_generation);
void d1l_meshcore_admin_runtime_note_mutation_tx(
    uint32_t request_generation, d1l_meshcore_admin_mutation_t mutation,
    uint32_t tag, esp_err_t result);
bool d1l_meshcore_admin_runtime_begin_cli(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    uint32_t tag, bool sensitive, bool read_only,
    d1l_meshcore_admin_cli_reply_profile_t reply_profile, uint64_t now_us,
    uint32_t *out_request_generation);
void d1l_meshcore_admin_runtime_note_cli_tx(
    uint32_t request_generation, uint32_t tag, esp_err_t result);
d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_runtime_dispatch_mutation_response(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    uint32_t response_timestamp, const uint8_t *text, size_t text_len,
    uint64_t now_us, bool *out_considered);
d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_runtime_dispatch_cli_response(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    uint32_t response_timestamp, const uint8_t *text, size_t text_len,
    uint64_t now_us, bool *out_considered);
d1l_meshcore_admin_response_result_t
d1l_meshcore_admin_runtime_dispatch_response(
    const d1l_meshcore_admin_binding_t *binding, uint32_t generation,
    const uint8_t *plaintext, size_t plaintext_len, uint64_t now_us,
    bool *out_considered);
bool d1l_meshcore_admin_runtime_expire(uint64_t now_us);
void d1l_meshcore_admin_runtime_snapshot(
    d1l_meshcore_admin_runtime_snapshot_t *out_snapshot);
void d1l_meshcore_admin_runtime_report_failure(esp_err_t reason);
void d1l_meshcore_admin_runtime_invalidate(esp_err_t reason);
void d1l_meshcore_admin_runtime_logout(void);

#ifdef __cplusplus
}
#endif
