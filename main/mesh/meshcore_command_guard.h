#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mesh/meshcore_runtime_guard.h"

/* MeshCore Admin currently permits at most fifteen credential bytes. Keep the
 * terminating byte in the guarded slot so no credential is copied into the
 * FreeRTOS command queue. meshcore_service.c asserts this capacity against the
 * protocol constant at compile time. */
#define D1L_MESH_COMMAND_ADMIN_PASSWORD_CAPACITY 16U
#define D1L_MESH_COMMAND_ADMIN_CLI_CAPACITY 161U
#define D1L_MESH_COMMAND_ADMIN_RESPONSE_FINGERPRINT_CAPACITY 17U
#define D1L_MESH_COMMAND_ADMIN_RESPONSE_PLAINTEXT_CAPACITY 256U

typedef struct {
    d1l_mesh_request_guard_t lifecycle;
    char admin_password[D1L_MESH_COMMAND_ADMIN_PASSWORD_CAPACITY];
    uint8_t admin_password_len;
    bool admin_password_present;
    char admin_cli[D1L_MESH_COMMAND_ADMIN_CLI_CAPACITY];
    uint16_t admin_cli_len;
    bool admin_cli_present;
} d1l_mesh_command_request_t;

typedef bool (*d1l_mesh_command_enqueue_fn_t)(
    void *context, const void *command);

typedef enum {
    D1L_MESH_COMMAND_ENQUEUE_INVALID = 0,
    D1L_MESH_COMMAND_ENQUEUE_QUEUED,
    D1L_MESH_COMMAND_ENQUEUE_SATURATED,
} d1l_mesh_command_enqueue_result_t;

typedef struct {
    d1l_mesh_request_guard_t lifecycle;
    uint64_t deadline_us;
    uint32_t session_generation;
    char fingerprint[
        D1L_MESH_COMMAND_ADMIN_RESPONSE_FINGERPRINT_CAPACITY];
    uint8_t plaintext[
        D1L_MESH_COMMAND_ADMIN_RESPONSE_PLAINTEXT_CAPACITY];
    uint16_t plaintext_len;
    bool count_rx_on_accept;
    bool present;
} d1l_mesh_admin_response_request_t;

typedef struct {
    const char *fingerprint;
    const uint8_t *plaintext;
    size_t plaintext_len;
    uint32_t session_generation;
    uint64_t deadline_us;
    bool count_rx_on_accept;
} d1l_mesh_admin_response_view_t;

typedef enum {
    D1L_MESH_ADMIN_RESPONSE_ADMIT_INVALID = 0,
    D1L_MESH_ADMIN_RESPONSE_ADMIT_OWNER_REQUIRED,
    D1L_MESH_ADMIN_RESPONSE_ADMIT_STALE_SESSION,
    D1L_MESH_ADMIN_RESPONSE_ADMIT_EXPIRED,
    D1L_MESH_ADMIN_RESPONSE_ADMIT_ACCEPTED,
} d1l_mesh_admin_response_admit_result_t;

static inline void d1l_mesh_command_secure_zero(void *value, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)value;
    size_t remaining = bytes ? size : 0U;
    while (remaining > 0U) {
        *bytes++ = 0U;
        remaining--;
    }
}

static inline void d1l_mesh_command_request_wipe_admin_password(
    d1l_mesh_command_request_t *request)
{
    if (!request) {
        return;
    }
    d1l_mesh_command_secure_zero(request->admin_password,
                                 sizeof(request->admin_password));
    request->admin_password_len = 0U;
    request->admin_password_present = false;
}

static inline void d1l_mesh_command_request_wipe_admin_cli(
    d1l_mesh_command_request_t *request)
{
    if (!request) {
        return;
    }
    d1l_mesh_command_secure_zero(request->admin_cli,
                                 sizeof(request->admin_cli));
    request->admin_cli_len = 0U;
    request->admin_cli_present = false;
}

static inline void d1l_mesh_command_request_wipe_admin_authority(
    d1l_mesh_command_request_t *request)
{
    d1l_mesh_command_request_wipe_admin_password(request);
    d1l_mesh_command_request_wipe_admin_cli(request);
}

static inline bool d1l_mesh_command_request_matches(
    const d1l_mesh_command_request_t *request, uint32_t request_id)
{
    return request &&
           d1l_mesh_request_guard_matches(&request->lifecycle, request_id);
}

static inline d1l_mesh_request_state_t d1l_mesh_command_request_state(
    const d1l_mesh_command_request_t *request)
{
    return request ? d1l_mesh_request_guard_state(&request->lifecycle) :
                     D1L_MESH_REQUEST_FREE;
}

static inline bool d1l_mesh_command_request_claim(
    d1l_mesh_command_request_t *request, uint32_t request_id)
{
    if (!request ||
        !d1l_mesh_request_guard_claim(&request->lifecycle, request_id)) {
        return false;
    }
    d1l_mesh_command_request_wipe_admin_authority(request);
    return true;
}

static inline bool d1l_mesh_command_request_publish(
    d1l_mesh_command_request_t *request, uint32_t request_id)
{
    return request && d1l_mesh_request_guard_publish(
                          &request->lifecycle, request_id);
}

static inline bool d1l_mesh_command_request_release(
    d1l_mesh_command_request_t *request, uint32_t request_id,
    d1l_mesh_request_state_t expected_state)
{
    if (!d1l_mesh_command_request_matches(request, request_id) ||
        d1l_mesh_command_request_state(request) != expected_state) {
        return false;
    }
    d1l_mesh_command_request_wipe_admin_authority(request);
    return d1l_mesh_request_guard_release(
        &request->lifecycle, request_id, expected_state);
}

static inline bool d1l_mesh_command_request_store_admin_password(
    d1l_mesh_command_request_t *request, uint32_t request_id,
    const char *password, size_t password_len)
{
    if (!password ||
        password_len >= D1L_MESH_COMMAND_ADMIN_PASSWORD_CAPACITY ||
        !d1l_mesh_command_request_matches(request, request_id) ||
        d1l_mesh_command_request_state(request) !=
            D1L_MESH_REQUEST_PENDING) {
        return false;
    }
    d1l_mesh_command_request_wipe_admin_authority(request);
    if (password_len > 0U) {
        memcpy(request->admin_password, password, password_len);
    }
    request->admin_password[password_len] = '\0';
    request->admin_password_len = (uint8_t)password_len;
    request->admin_password_present = true;
    return true;
}

static inline bool d1l_mesh_command_request_take_admin_password(
    d1l_mesh_command_request_t *request, uint32_t request_id,
    char *out_password, size_t out_size, size_t *out_len)
{
    if (!out_password || !out_len ||
        !d1l_mesh_command_request_matches(request, request_id) ||
        d1l_mesh_command_request_state(request) !=
            D1L_MESH_REQUEST_ADMITTED) {
        return false;
    }
    const size_t password_len = request->admin_password_len;
    const bool valid = request->admin_password_present &&
        password_len < D1L_MESH_COMMAND_ADMIN_PASSWORD_CAPACITY &&
        out_size > password_len &&
        request->admin_password[password_len] == '\0';
    if (valid) {
        memcpy(out_password, request->admin_password, password_len + 1U);
        *out_len = password_len;
    }
    d1l_mesh_command_request_wipe_admin_authority(request);
    return valid;
}

static inline bool d1l_mesh_command_request_store_admin_cli(
    d1l_mesh_command_request_t *request, uint32_t request_id,
    const char *command, size_t command_len)
{
    if (!command || command_len == 0U ||
        command_len >= D1L_MESH_COMMAND_ADMIN_CLI_CAPACITY ||
        !d1l_mesh_command_request_matches(request, request_id) ||
        d1l_mesh_command_request_state(request) !=
            D1L_MESH_REQUEST_PENDING) {
        return false;
    }
    d1l_mesh_command_request_wipe_admin_authority(request);
    memcpy(request->admin_cli, command, command_len);
    request->admin_cli[command_len] = '\0';
    request->admin_cli_len = (uint16_t)command_len;
    request->admin_cli_present = true;
    return true;
}

static inline bool d1l_mesh_command_request_take_admin_cli(
    d1l_mesh_command_request_t *request, uint32_t request_id,
    char *out_command, size_t out_size, size_t *out_len)
{
    if (!out_command || !out_len ||
        !d1l_mesh_command_request_matches(request, request_id) ||
        d1l_mesh_command_request_state(request) !=
            D1L_MESH_REQUEST_ADMITTED) {
        return false;
    }
    const size_t command_len = request->admin_cli_len;
    const bool valid = request->admin_cli_present &&
        command_len > 0U &&
        command_len < D1L_MESH_COMMAND_ADMIN_CLI_CAPACITY &&
        out_size > command_len &&
        request->admin_cli[command_len] == '\0';
    if (valid) {
        memcpy(out_command, request->admin_cli, command_len + 1U);
        *out_len = command_len;
    }
    d1l_mesh_command_request_wipe_admin_authority(request);
    return valid;
}

static inline bool d1l_mesh_command_request_cancel_and_release(
    d1l_mesh_command_request_t *request, uint32_t request_id)
{
    if (!request || !d1l_mesh_request_guard_transition(
            &request->lifecycle, request_id, D1L_MESH_REQUEST_PENDING,
            D1L_MESH_REQUEST_CALLER_CANCELLED)) {
        return false;
    }
    d1l_mesh_command_request_wipe_admin_authority(request);
    return d1l_mesh_request_guard_release(
        &request->lifecycle, request_id,
        D1L_MESH_REQUEST_CALLER_CANCELLED);
}

static inline bool d1l_mesh_command_request_expire(
    d1l_mesh_command_request_t *request, uint32_t request_id)
{
    if (!request || !d1l_mesh_request_guard_transition(
            &request->lifecycle, request_id, D1L_MESH_REQUEST_PENDING,
            D1L_MESH_REQUEST_OWNER_EXPIRED)) {
        return false;
    }
    d1l_mesh_command_request_wipe_admin_authority(request);
    return true;
}

static inline bool d1l_mesh_command_request_admit(
    d1l_mesh_command_request_t *request, uint32_t request_id)
{
    return request && d1l_mesh_request_guard_transition(
                          &request->lifecycle, request_id,
                          D1L_MESH_REQUEST_PENDING,
                          D1L_MESH_REQUEST_ADMITTED);
}

static inline bool d1l_mesh_command_request_complete(
    d1l_mesh_command_request_t *request, uint32_t request_id)
{
    if (!d1l_mesh_command_request_matches(request, request_id) ||
        d1l_mesh_command_request_state(request) !=
            D1L_MESH_REQUEST_ADMITTED) {
        return false;
    }
    /* An admitted request is exclusively owner-held until completion is
     * signalled, so wiping before the terminal CAS cannot race slot reuse. */
    d1l_mesh_command_request_wipe_admin_authority(request);
    return d1l_mesh_request_guard_transition(
        &request->lifecycle, request_id, D1L_MESH_REQUEST_ADMITTED,
        D1L_MESH_REQUEST_COMPLETED);
}

static inline d1l_mesh_command_enqueue_result_t
d1l_mesh_command_request_enqueue(
    d1l_mesh_command_request_t *request, uint32_t request_id,
    d1l_mesh_command_enqueue_fn_t enqueue, void *enqueue_context,
    const void *command)
{
    if (!enqueue || !command ||
        !d1l_mesh_command_request_matches(request, request_id) ||
        d1l_mesh_command_request_state(request) !=
            D1L_MESH_REQUEST_PENDING) {
        return D1L_MESH_COMMAND_ENQUEUE_INVALID;
    }
    if (enqueue(enqueue_context, command)) {
        return D1L_MESH_COMMAND_ENQUEUE_QUEUED;
    }
    return d1l_mesh_command_request_cancel_and_release(request, request_id) ?
        D1L_MESH_COMMAND_ENQUEUE_SATURATED :
        D1L_MESH_COMMAND_ENQUEUE_INVALID;
}

static inline bool d1l_mesh_command_sync_wait_allowed(
    const void *owner, const void *caller)
{
    return !owner || owner != caller;
}

static inline void d1l_mesh_admin_response_request_wipe(
    d1l_mesh_admin_response_request_t *request)
{
    if (!request) {
        return;
    }
    d1l_mesh_command_secure_zero(
        request->fingerprint, sizeof(request->fingerprint));
    d1l_mesh_command_secure_zero(
        request->plaintext, sizeof(request->plaintext));
    request->deadline_us = 0U;
    request->session_generation = 0U;
    request->plaintext_len = 0U;
    request->count_rx_on_accept = false;
    request->present = false;
}

static inline bool d1l_mesh_admin_response_request_claim(
    d1l_mesh_admin_response_request_t *request, uint32_t request_id)
{
    if (!request ||
        !d1l_mesh_request_guard_claim(&request->lifecycle, request_id)) {
        return false;
    }
    d1l_mesh_admin_response_request_wipe(request);
    return true;
}

static inline bool d1l_mesh_admin_response_request_publish(
    d1l_mesh_admin_response_request_t *request, uint32_t request_id,
    const char *fingerprint, uint32_t session_generation,
    const uint8_t *plaintext, size_t plaintext_len,
    bool count_rx_on_accept, uint64_t deadline_us)
{
    size_t fingerprint_len = 0U;
    if (fingerprint) {
        while (fingerprint_len <
                   D1L_MESH_COMMAND_ADMIN_RESPONSE_FINGERPRINT_CAPACITY &&
               fingerprint[fingerprint_len] != '\0') {
            fingerprint_len++;
        }
    }
    const bool valid =
        request && request_id != 0U && session_generation != 0U &&
        deadline_us != 0U && plaintext && plaintext_len > 0U &&
        plaintext_len <=
            D1L_MESH_COMMAND_ADMIN_RESPONSE_PLAINTEXT_CAPACITY &&
        fingerprint_len > 0U &&
        fingerprint_len <
            D1L_MESH_COMMAND_ADMIN_RESPONSE_FINGERPRINT_CAPACITY &&
        d1l_mesh_request_guard_matches(
            &request->lifecycle, request_id) &&
        d1l_mesh_request_guard_state(&request->lifecycle) ==
            D1L_MESH_REQUEST_CLAIMED;
    if (!valid) {
        if (request &&
            d1l_mesh_request_guard_matches(
                &request->lifecycle, request_id) &&
            d1l_mesh_request_guard_state(&request->lifecycle) ==
                D1L_MESH_REQUEST_CLAIMED) {
            d1l_mesh_admin_response_request_wipe(request);
            (void)d1l_mesh_request_guard_release(
                &request->lifecycle, request_id,
                D1L_MESH_REQUEST_CLAIMED);
        }
        return false;
    }

    memcpy(request->fingerprint, fingerprint, fingerprint_len);
    request->fingerprint[fingerprint_len] = '\0';
    memcpy(request->plaintext, plaintext, plaintext_len);
    request->plaintext_len = (uint16_t)plaintext_len;
    request->session_generation = session_generation;
    request->deadline_us = deadline_us;
    request->count_rx_on_accept = count_rx_on_accept;
    request->present = true;
    if (d1l_mesh_request_guard_publish(
            &request->lifecycle, request_id)) {
        return true;
    }
    d1l_mesh_admin_response_request_wipe(request);
    (void)d1l_mesh_request_guard_release(
        &request->lifecycle, request_id, D1L_MESH_REQUEST_CLAIMED);
    return false;
}

static inline bool d1l_mesh_admin_response_request_cancel(
    d1l_mesh_admin_response_request_t *request, uint32_t request_id)
{
    if (!request || !d1l_mesh_request_guard_transition(
            &request->lifecycle, request_id,
            D1L_MESH_REQUEST_PENDING,
            D1L_MESH_REQUEST_CALLER_CANCELLED)) {
        return false;
    }
    d1l_mesh_admin_response_request_wipe(request);
    return d1l_mesh_request_guard_release(
        &request->lifecycle, request_id,
        D1L_MESH_REQUEST_CALLER_CANCELLED);
}

static inline d1l_mesh_command_enqueue_result_t
d1l_mesh_admin_response_request_enqueue(
    d1l_mesh_admin_response_request_t *request, uint32_t request_id,
    d1l_mesh_command_enqueue_fn_t enqueue, void *enqueue_context,
    const void *command)
{
    if (!enqueue || !command || !request ||
        !d1l_mesh_request_guard_matches(
            &request->lifecycle, request_id) ||
        d1l_mesh_request_guard_state(&request->lifecycle) !=
            D1L_MESH_REQUEST_PENDING) {
        return D1L_MESH_COMMAND_ENQUEUE_INVALID;
    }
    if (enqueue(enqueue_context, command)) {
        return D1L_MESH_COMMAND_ENQUEUE_QUEUED;
    }
    if (d1l_mesh_admin_response_request_cancel(request, request_id)) {
        return D1L_MESH_COMMAND_ENQUEUE_SATURATED;
    }
    return D1L_MESH_COMMAND_ENQUEUE_INVALID;
}

/* Only the runtime owner may reap queued Admin response bytes. Snapshot and
 * CAS the complete request-id/state word so a delayed sweep for generation A
 * cannot expire a generation B request that reused the same slot. Once the
 * exact PENDING word becomes OWNER_EXPIRED, the owner wipes all response
 * material before releasing that exact lifecycle word back to FREE. */
static inline bool d1l_mesh_admin_response_request_owner_reap(
    d1l_mesh_admin_response_request_t *request, bool caller_is_owner,
    uint32_t current_session_generation, uint64_t now_us)
{
    if (!request || !caller_is_owner) {
        return false;
    }

    const uint32_t observed = __atomic_load_n(
        &request->lifecycle.word, __ATOMIC_ACQUIRE);
    const uint32_t request_id =
        observed >> D1L_MESH_REQUEST_STATE_BITS;
    const d1l_mesh_request_state_t state =
        (d1l_mesh_request_state_t)(
            observed & D1L_MESH_REQUEST_STATE_MASK);
    if (request_id == 0U || state != D1L_MESH_REQUEST_PENDING) {
        return false;
    }

    const bool due = request->deadline_us == 0U ||
        now_us >= request->deadline_us;
    const bool stale = request->session_generation !=
        current_session_generation;
    if (!due && !stale) {
        return false;
    }

    uint32_t expected = observed;
    const uint32_t expired = d1l_mesh_request_guard_word(
        request_id, D1L_MESH_REQUEST_OWNER_EXPIRED);
    if (!__atomic_compare_exchange_n(
            &request->lifecycle.word, &expected, expired, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return false;
    }
    d1l_mesh_admin_response_request_wipe(request);
    return d1l_mesh_request_guard_release(
        &request->lifecycle, request_id,
        D1L_MESH_REQUEST_OWNER_EXPIRED);
}

static inline d1l_mesh_admin_response_admit_result_t
d1l_mesh_admin_response_request_admit(
    d1l_mesh_admin_response_request_t *request, uint32_t request_id,
    bool caller_is_owner, uint32_t current_session_generation,
    uint64_t now_us, d1l_mesh_admin_response_view_t *out_view)
{
    if (out_view) {
        memset(out_view, 0, sizeof(*out_view));
    }
    if (!caller_is_owner) {
        return D1L_MESH_ADMIN_RESPONSE_ADMIT_OWNER_REQUIRED;
    }
    if (!request || !out_view ||
        !d1l_mesh_request_guard_matches(
            &request->lifecycle, request_id) ||
        !d1l_mesh_request_guard_transition(
            &request->lifecycle, request_id,
            D1L_MESH_REQUEST_PENDING,
            D1L_MESH_REQUEST_ADMITTED)) {
        return D1L_MESH_ADMIN_RESPONSE_ADMIT_INVALID;
    }

    d1l_mesh_admin_response_admit_result_t result =
        D1L_MESH_ADMIN_RESPONSE_ADMIT_INVALID;
    const bool structurally_valid =
        request->present && request->session_generation != 0U &&
        request->deadline_us != 0U && request->fingerprint[0] != '\0' &&
        request->plaintext_len > 0U &&
        request->plaintext_len <=
            D1L_MESH_COMMAND_ADMIN_RESPONSE_PLAINTEXT_CAPACITY;
    if (structurally_valid && now_us >= request->deadline_us) {
        result = D1L_MESH_ADMIN_RESPONSE_ADMIT_EXPIRED;
    } else if (structurally_valid &&
               request->session_generation !=
                   current_session_generation) {
        result = D1L_MESH_ADMIN_RESPONSE_ADMIT_STALE_SESSION;
    } else if (structurally_valid) {
        out_view->fingerprint = request->fingerprint;
        out_view->plaintext = request->plaintext;
        out_view->plaintext_len = request->plaintext_len;
        out_view->session_generation = request->session_generation;
        out_view->deadline_us = request->deadline_us;
        out_view->count_rx_on_accept = request->count_rx_on_accept;
        return D1L_MESH_ADMIN_RESPONSE_ADMIT_ACCEPTED;
    }

    d1l_mesh_admin_response_request_wipe(request);
    (void)d1l_mesh_request_guard_release(
        &request->lifecycle, request_id, D1L_MESH_REQUEST_ADMITTED);
    return result;
}

static inline bool d1l_mesh_admin_response_request_complete(
    d1l_mesh_admin_response_request_t *request, uint32_t request_id)
{
    if (!request ||
        !d1l_mesh_request_guard_matches(
            &request->lifecycle, request_id) ||
        d1l_mesh_request_guard_state(&request->lifecycle) !=
            D1L_MESH_REQUEST_ADMITTED) {
        return false;
    }
    d1l_mesh_admin_response_request_wipe(request);
    return d1l_mesh_request_guard_release(
        &request->lifecycle, request_id, D1L_MESH_REQUEST_ADMITTED);
}
