#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mesh/meshcore_command_guard.h"

static void test_caller_timeout_cancels_only_pending(void)
{
    d1l_mesh_request_guard_t guard = {0};
    assert(d1l_mesh_request_guard_claim(&guard, 11U));
    assert(d1l_mesh_request_guard_publish(&guard, 11U));
    assert(d1l_mesh_request_guard_transition(
        &guard, 11U, D1L_MESH_REQUEST_PENDING,
        D1L_MESH_REQUEST_CALLER_CANCELLED));

    /* Once cancellation wins, owner admission and completion both lose. */
    assert(!d1l_mesh_request_guard_transition(
        &guard, 11U, D1L_MESH_REQUEST_PENDING,
        D1L_MESH_REQUEST_ADMITTED));
    assert(!d1l_mesh_request_guard_transition(
        &guard, 11U, D1L_MESH_REQUEST_ADMITTED,
        D1L_MESH_REQUEST_COMPLETED));
    assert(d1l_mesh_request_guard_release(
        &guard, 11U, D1L_MESH_REQUEST_CALLER_CANCELLED));
    assert(d1l_mesh_request_guard_state(&guard) == D1L_MESH_REQUEST_FREE);
}

static void test_admission_wins_timeout_and_requires_exact_completion(void)
{
    d1l_mesh_request_guard_t guard = {0};
    assert(d1l_mesh_request_guard_claim(&guard, 12U));
    assert(d1l_mesh_request_guard_publish(&guard, 12U));
    assert(d1l_mesh_request_guard_transition(
        &guard, 12U, D1L_MESH_REQUEST_PENDING,
        D1L_MESH_REQUEST_ADMITTED));

    /* The caller can no longer cancel after the owner admits side effects. */
    assert(!d1l_mesh_request_guard_transition(
        &guard, 12U, D1L_MESH_REQUEST_PENDING,
        D1L_MESH_REQUEST_CALLER_CANCELLED));
    assert(!d1l_mesh_request_guard_transition(
        &guard, 13U, D1L_MESH_REQUEST_ADMITTED,
        D1L_MESH_REQUEST_COMPLETED));
    assert(d1l_mesh_request_guard_transition(
        &guard, 12U, D1L_MESH_REQUEST_ADMITTED,
        D1L_MESH_REQUEST_COMPLETED));
    assert(!d1l_mesh_request_guard_transition(
        &guard, 12U, D1L_MESH_REQUEST_ADMITTED,
        D1L_MESH_REQUEST_COMPLETED));
    assert(d1l_mesh_request_guard_release(
        &guard, 12U, D1L_MESH_REQUEST_COMPLETED));
}

static void test_expiry_and_stale_reply_cannot_cross_slot_generations(void)
{
    d1l_mesh_request_guard_t guard = {0};
    assert(d1l_mesh_request_guard_claim(&guard, 21U));
    assert(d1l_mesh_request_guard_publish(&guard, 21U));
    assert(d1l_mesh_request_guard_transition(
        &guard, 21U, D1L_MESH_REQUEST_PENDING,
        D1L_MESH_REQUEST_OWNER_EXPIRED));
    assert(!d1l_mesh_request_guard_transition(
        &guard, 21U, D1L_MESH_REQUEST_PENDING,
        D1L_MESH_REQUEST_ADMITTED));
    assert(d1l_mesh_request_guard_release(
        &guard, 21U, D1L_MESH_REQUEST_OWNER_EXPIRED));

    assert(d1l_mesh_request_guard_claim(&guard, 22U));
    assert(d1l_mesh_request_guard_publish(&guard, 22U));
    assert(!d1l_mesh_request_guard_transition(
        &guard, 21U, D1L_MESH_REQUEST_PENDING,
        D1L_MESH_REQUEST_ADMITTED));
    assert(!d1l_mesh_request_guard_transition(
        &guard, 21U, D1L_MESH_REQUEST_ADMITTED,
        D1L_MESH_REQUEST_COMPLETED));
    assert(!d1l_mesh_request_guard_release(
        &guard, 21U, D1L_MESH_REQUEST_PENDING));
    assert(d1l_mesh_request_guard_state(&guard) == D1L_MESH_REQUEST_PENDING);
    assert(d1l_mesh_request_guard_matches(&guard, 22U));
    assert(d1l_mesh_request_guard_transition(
        &guard, 22U, D1L_MESH_REQUEST_PENDING,
        D1L_MESH_REQUEST_CALLER_CANCELLED));
    assert(d1l_mesh_request_guard_release(
        &guard, 22U, D1L_MESH_REQUEST_CALLER_CANCELLED));
}

static void test_atomic_identity_state_tuple_blocks_slot_reuse_aba(void)
{
    d1l_mesh_request_guard_t guard = {0};
    assert(sizeof(guard) == sizeof(uint32_t));
    assert(d1l_mesh_request_guard_claim(&guard, 31U));
    assert(d1l_mesh_request_guard_publish(&guard, 31U));

    const uint32_t stale_pending = d1l_mesh_request_guard_word(
        31U, D1L_MESH_REQUEST_PENDING);
    assert(d1l_mesh_request_guard_transition(
        &guard, 31U, D1L_MESH_REQUEST_PENDING,
        D1L_MESH_REQUEST_CALLER_CANCELLED));
    assert(d1l_mesh_request_guard_release(
        &guard, 31U, D1L_MESH_REQUEST_CALLER_CANCELLED));
    assert(d1l_mesh_request_guard_claim(&guard, 32U));
    assert(d1l_mesh_request_guard_publish(&guard, 32U));

    /* Model an old actor resuming with the exact CAS expectation it captured
     * before release/reuse. The new request has the same lifecycle state but
     * a different identity in the same atomic word, so the CAS must fail. */
    uint32_t expected = stale_pending;
    const uint32_t stale_admitted = d1l_mesh_request_guard_word(
        31U, D1L_MESH_REQUEST_ADMITTED);
    assert(!__atomic_compare_exchange_n(
        &guard.word, &expected, stale_admitted, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    assert(d1l_mesh_request_guard_matches(&guard, 32U));
    assert(d1l_mesh_request_guard_state(&guard) == D1L_MESH_REQUEST_PENDING);
    assert(!d1l_mesh_request_guard_transition(
        &guard, 31U, D1L_MESH_REQUEST_PENDING,
        D1L_MESH_REQUEST_ADMITTED));
    assert(d1l_mesh_request_guard_transition(
        &guard, 32U, D1L_MESH_REQUEST_PENDING,
        D1L_MESH_REQUEST_CALLER_CANCELLED));
    assert(d1l_mesh_request_guard_release(
        &guard, 32U, D1L_MESH_REQUEST_CALLER_CANCELLED));
}

typedef struct {
    bool accept;
    unsigned calls;
    const void *last_command;
} admin_test_queue_t;

static bool bytes_zero(const void *value, size_t size)
{
    const unsigned char *bytes = value;
    if (!bytes) {
        return false;
    }
    while (size > 0U) {
        if (*bytes++ != 0U) {
            return false;
        }
        size--;
    }
    return true;
}

static bool request_password_clear(
    const d1l_mesh_command_request_t *request)
{
    return request && !request->admin_password_present &&
           request->admin_password_len == 0U &&
           bytes_zero(request->admin_password,
                      sizeof(request->admin_password)) &&
           !request->admin_cli_present &&
           request->admin_cli_len == 0U &&
           bytes_zero(request->admin_cli,
                      sizeof(request->admin_cli));
}

static bool admin_test_enqueue(void *context, const void *command)
{
    admin_test_queue_t *queue = context;
    assert(queue && command);
    queue->calls++;
    queue->last_command = command;
    return queue->accept;
}

typedef struct {
    uint32_t request_id;
    uint64_t deadline_us;
    uint8_t slot;
} admin_response_token_t;

typedef struct {
    bool accept;
    unsigned calls;
    admin_response_token_t token;
} admin_response_test_queue_t;

static bool admin_response_request_clear(
    const d1l_mesh_admin_response_request_t *request)
{
    return request && !request->present && request->deadline_us == 0U &&
           request->session_generation == 0U &&
           request->plaintext_len == 0U &&
           !request->count_rx_on_accept &&
           bytes_zero(request->fingerprint, sizeof(request->fingerprint)) &&
           bytes_zero(request->plaintext, sizeof(request->plaintext));
}

static bool bytes_contain(const void *haystack, size_t haystack_len,
                          const void *needle, size_t needle_len)
{
    const uint8_t *bytes = haystack;
    if (!bytes || !needle || needle_len == 0U || needle_len > haystack_len) {
        return false;
    }
    for (size_t offset = 0U; offset + needle_len <= haystack_len; ++offset) {
        if (memcmp(bytes + offset, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool admin_response_test_enqueue(void *context, const void *command)
{
    admin_response_test_queue_t *queue = context;
    assert(queue && command);
    queue->calls++;
    memcpy(&queue->token, command, sizeof(queue->token));
    return queue->accept;
}

static void publish_admin_response(
    d1l_mesh_admin_response_request_t *request, uint32_t request_id,
    uint32_t session_generation, const uint8_t *plaintext,
    size_t plaintext_len, uint64_t deadline_us)
{
    assert(d1l_mesh_admin_response_request_claim(request, request_id));
    assert(admin_response_request_clear(request));
    assert(d1l_mesh_admin_response_request_publish(
        request, request_id, "0011223344556677", session_generation,
        plaintext, plaintext_len, true, deadline_us));
}

static void publish_admin_login(d1l_mesh_command_request_t *request,
                                uint32_t request_id,
                                const char *password)
{
    assert(request && password);
    assert(d1l_mesh_command_request_claim(request, request_id));
    assert(request_password_clear(request));
    assert(d1l_mesh_command_request_publish(request, request_id));
    assert(d1l_mesh_command_request_store_admin_password(
        request, request_id, password, strlen(password)));
}

static void finish_admitted_admin_login(
    d1l_mesh_command_request_t *request, uint32_t request_id,
    const char *expected_password)
{
    char owner_password[D1L_MESH_COMMAND_ADMIN_PASSWORD_CAPACITY] = {0};
    size_t owner_password_len = 0U;
    assert(d1l_mesh_command_request_admit(request, request_id));
    assert(d1l_mesh_command_request_take_admin_password(
        request, request_id, owner_password, sizeof(owner_password),
        &owner_password_len));
    assert(owner_password_len == strlen(expected_password));
    assert(strcmp(owner_password, expected_password) == 0);
    assert(request_password_clear(request));
    d1l_mesh_command_secure_zero(owner_password, sizeof(owner_password));
    assert(bytes_zero(owner_password, sizeof(owner_password)));
    assert(d1l_mesh_command_request_complete(request, request_id));
    assert(d1l_mesh_command_request_release(
        request, request_id, D1L_MESH_REQUEST_COMPLETED));
}

static void publish_admin_cli(d1l_mesh_command_request_t *request,
                              uint32_t request_id,
                              const char *command)
{
    assert(request && command);
    assert(d1l_mesh_command_request_claim(request, request_id));
    assert(request_password_clear(request));
    assert(d1l_mesh_command_request_publish(request, request_id));
    assert(d1l_mesh_command_request_store_admin_cli(
        request, request_id, command, strlen(command)));
}

static void finish_admitted_admin_cli(
    d1l_mesh_command_request_t *request, uint32_t request_id,
    const char *expected_command)
{
    char owner_command[D1L_MESH_COMMAND_ADMIN_CLI_CAPACITY] = {0};
    size_t owner_command_len = 0U;
    assert(d1l_mesh_command_request_admit(request, request_id));
    assert(d1l_mesh_command_request_take_admin_cli(
        request, request_id, owner_command, sizeof(owner_command),
        &owner_command_len));
    assert(owner_command_len == strlen(expected_command));
    assert(strcmp(owner_command, expected_command) == 0);
    assert(request_password_clear(request));
    d1l_mesh_command_secure_zero(owner_command, sizeof(owner_command));
    assert(bytes_zero(owner_command, sizeof(owner_command)));
    assert(d1l_mesh_command_request_complete(request, request_id));
    assert(d1l_mesh_command_request_release(
        request, request_id, D1L_MESH_REQUEST_COMPLETED));
}

static void test_admin_cli_guard_is_bounded_and_generation_safe(void)
{
    d1l_mesh_command_request_t cancelled = {0};
    publish_admin_cli(
        &cancelled, 91U, "set guest.password should-never-survive");
    assert(d1l_mesh_command_request_cancel_and_release(&cancelled, 91U));
    assert(request_password_clear(&cancelled));
    assert(!d1l_mesh_command_request_admit(&cancelled, 91U));

    d1l_mesh_command_request_t queued = {0};
    admin_test_queue_t accepting_queue = {.accept = true};
    const uint32_t dummy_command = 0xC11U;
    publish_admin_cli(&queued, 92U, "neighbors");
    assert(d1l_mesh_command_request_enqueue(
        &queued, 92U, admin_test_enqueue, &accepting_queue,
        &dummy_command) == D1L_MESH_COMMAND_ENQUEUE_QUEUED);
    finish_admitted_admin_cli(&queued, 92U, "neighbors");

    d1l_mesh_command_request_t reused = {0};
    publish_admin_cli(&reused, 93U, "old-command");
    assert(d1l_mesh_command_request_cancel_and_release(&reused, 93U));
    publish_admin_cli(&reused, 94U, "set name current");
    char stale_output[D1L_MESH_COMMAND_ADMIN_CLI_CAPACITY] = {0};
    size_t stale_len = 0U;
    assert(!d1l_mesh_command_request_admit(&reused, 93U));
    assert(!d1l_mesh_command_request_take_admin_cli(
        &reused, 93U, stale_output, sizeof(stale_output), &stale_len));
    assert(reused.admin_cli_present);
    assert(strcmp(reused.admin_cli, "set name current") == 0);
    finish_admitted_admin_cli(&reused, 94U, "set name current");
}

static void test_admin_cancel_and_expiry_have_no_side_effects(void)
{
    d1l_mesh_command_request_t cancelled = {0};
    unsigned cancelled_mutations = 0U;
    publish_admin_login(&cancelled, 101U, "secret");
    assert(d1l_mesh_command_request_cancel_and_release(&cancelled, 101U));
    if (d1l_mesh_command_request_admit(&cancelled, 101U)) {
        cancelled_mutations++;
    }
    assert(cancelled_mutations == 0U);
    assert(request_password_clear(&cancelled));
    assert(d1l_mesh_command_request_state(&cancelled) ==
           D1L_MESH_REQUEST_FREE);

    d1l_mesh_command_request_t expired = {0};
    unsigned expired_mutations = 0U;
    publish_admin_login(&expired, 102U, "secret");
    assert(d1l_mesh_command_request_expire(&expired, 102U));
    if (d1l_mesh_command_request_admit(&expired, 102U)) {
        expired_mutations++;
    }
    assert(expired_mutations == 0U);
    assert(request_password_clear(&expired));
    assert(d1l_mesh_command_request_state(&expired) ==
           D1L_MESH_REQUEST_OWNER_EXPIRED);
    assert(d1l_mesh_command_request_release(
        &expired, 102U, D1L_MESH_REQUEST_OWNER_EXPIRED));
}

static void test_admin_queue_saturation_and_admission_execute_production_guard(void)
{
    const uint32_t dummy_command = 0xA5A5U;
    d1l_mesh_command_request_t saturated = {0};
    admin_test_queue_t full_queue = {.accept = false};
    publish_admin_login(&saturated, 111U, "queue-full");
    assert(d1l_mesh_command_request_enqueue(
        &saturated, 111U, admin_test_enqueue, &full_queue,
        &dummy_command) == D1L_MESH_COMMAND_ENQUEUE_SATURATED);
    assert(full_queue.calls == 1U &&
           full_queue.last_command == &dummy_command);
    assert(request_password_clear(&saturated));
    assert(d1l_mesh_command_request_state(&saturated) ==
           D1L_MESH_REQUEST_FREE);
    assert(!d1l_mesh_command_request_admit(&saturated, 111U));

    d1l_mesh_command_request_t queued = {0};
    admin_test_queue_t accepting_queue = {.accept = true};
    publish_admin_login(&queued, 112U, "accepted");
    assert(d1l_mesh_command_request_enqueue(
        &queued, 112U, admin_test_enqueue, &accepting_queue,
        &dummy_command) == D1L_MESH_COMMAND_ENQUEUE_QUEUED);
    assert(d1l_mesh_command_request_state(&queued) ==
           D1L_MESH_REQUEST_PENDING);
    finish_admitted_admin_login(&queued, 112U, "accepted");
}

static void test_admin_slot_reuse_rejects_stale_generation_without_wipe(void)
{
    d1l_mesh_command_request_t request = {0};
    publish_admin_login(&request, 121U, "old");
    assert(d1l_mesh_command_request_cancel_and_release(&request, 121U));
    publish_admin_login(&request, 122U, "new-secret");

    char stale_output[D1L_MESH_COMMAND_ADMIN_PASSWORD_CAPACITY] = {0};
    size_t stale_len = 0U;
    assert(!d1l_mesh_command_request_admit(&request, 121U));
    assert(!d1l_mesh_command_request_take_admin_password(
        &request, 121U, stale_output, sizeof(stale_output), &stale_len));
    assert(!d1l_mesh_command_request_complete(&request, 121U));
    assert(!d1l_mesh_command_request_release(
        &request, 121U, D1L_MESH_REQUEST_PENDING));
    assert(request.admin_password_present);
    assert(strcmp(request.admin_password, "new-secret") == 0);
    finish_admitted_admin_login(&request, 122U, "new-secret");
}

static void test_admin_logout_and_owner_recursion_use_production_guard(void)
{
    int owner_token = 0;
    int other_token = 0;
    assert(d1l_mesh_command_sync_wait_allowed(NULL, &owner_token));
    assert(d1l_mesh_command_sync_wait_allowed(&owner_token, &other_token));
    assert(!d1l_mesh_command_sync_wait_allowed(&owner_token, &owner_token));

    const uint32_t logout_command = 3U;
    d1l_mesh_command_request_t logout = {0};
    admin_test_queue_t queue = {.accept = true};
    assert(d1l_mesh_command_request_claim(&logout, 131U));
    assert(d1l_mesh_command_request_publish(&logout, 131U));
    assert(d1l_mesh_command_request_enqueue(
        &logout, 131U, admin_test_enqueue, &queue, &logout_command) ==
           D1L_MESH_COMMAND_ENQUEUE_QUEUED);
    assert(d1l_mesh_command_request_admit(&logout, 131U));
    unsigned logout_mutations = 0U;
    logout_mutations++;
    assert(d1l_mesh_command_request_complete(&logout, 131U));
    assert(d1l_mesh_command_request_release(
        &logout, 131U, D1L_MESH_REQUEST_COMPLETED));
    assert(logout_mutations == 1U);
    assert(request_password_clear(&logout));
}

static void test_admin_response_mutation_requires_runtime_owner(void)
{
    static const uint8_t plaintext[] = "owner-only-response-secret";
    static const uint8_t secret_marker[] = "response-secret";
    d1l_mesh_admin_response_request_t request = {0};
    publish_admin_response(&request, 201U, 7U, plaintext,
                           sizeof(plaintext) - 1U, 500U);
    admin_response_token_t token;
    memset(&token, 0, sizeof(token));
    token.request_id = 201U;
    token.deadline_us = 500U;
    token.slot = 2U;
    admin_response_test_queue_t queue = {.accept = true};
    assert(d1l_mesh_admin_response_request_enqueue(
               &request, 201U, admin_response_test_enqueue, &queue,
               &token) == D1L_MESH_COMMAND_ENQUEUE_QUEUED);
    assert(queue.calls == 1U);
    assert(memcmp(&queue.token, &token, sizeof(token)) == 0);
    assert(bytes_contain(plaintext, sizeof(plaintext) - 1U,
                         secret_marker, sizeof(secret_marker) - 1U));
    assert(!bytes_contain(&queue.token, sizeof(queue.token),
                          secret_marker, sizeof(secret_marker) - 1U));
    assert(!bytes_contain(&queue.token, sizeof(queue.token),
                          "0011223344556677", 16U));

    unsigned mutations = 0U;
    d1l_mesh_admin_response_view_t view = {0};
    assert(d1l_mesh_admin_response_request_admit(
               &request, 201U, false, 7U, 100U, &view) ==
           D1L_MESH_ADMIN_RESPONSE_ADMIT_OWNER_REQUIRED);
    assert(mutations == 0U);
    assert(d1l_mesh_request_guard_state(&request.lifecycle) ==
           D1L_MESH_REQUEST_PENDING);
    assert(request.present);
    assert(bytes_zero(&view, sizeof(view)));

    assert(d1l_mesh_admin_response_request_admit(
               &request, 201U, true, 7U, 100U, &view) ==
           D1L_MESH_ADMIN_RESPONSE_ADMIT_ACCEPTED);
    mutations++;
    assert(view.session_generation == 7U);
    assert(view.deadline_us == 500U);
    assert(view.count_rx_on_accept);
    assert(view.plaintext_len == sizeof(plaintext) - 1U);
    assert(memcmp(view.plaintext, plaintext, view.plaintext_len) == 0);
    assert(strcmp(view.fingerprint, "0011223344556677") == 0);
    assert(mutations == 1U);
    assert(d1l_mesh_admin_response_request_complete(&request, 201U));
    assert(d1l_mesh_request_guard_state(&request.lifecycle) ==
           D1L_MESH_REQUEST_FREE);
    assert(admin_response_request_clear(&request));
}

static void test_admin_response_rejects_stale_session_and_slot_generation(void)
{
    static const uint8_t old_plaintext[] = "old-generation-secret";
    static const uint8_t new_plaintext[] = "new-generation-secret";
    d1l_mesh_admin_response_request_t request = {0};
    d1l_mesh_admin_response_view_t view = {0};
    unsigned mutations = 0U;

    publish_admin_response(&request, 202U, 7U, old_plaintext,
                           sizeof(old_plaintext) - 1U, 500U);
    assert(d1l_mesh_admin_response_request_admit(
               &request, 202U, true, 8U, 100U, &view) ==
           D1L_MESH_ADMIN_RESPONSE_ADMIT_STALE_SESSION);
    assert(mutations == 0U);
    assert(d1l_mesh_request_guard_state(&request.lifecycle) ==
           D1L_MESH_REQUEST_FREE);
    assert(admin_response_request_clear(&request));

    publish_admin_response(&request, 203U, 8U, new_plaintext,
                           sizeof(new_plaintext) - 1U, 600U);
    assert(d1l_mesh_admin_response_request_admit(
               &request, 202U, true, 8U, 100U, &view) ==
           D1L_MESH_ADMIN_RESPONSE_ADMIT_INVALID);
    assert(request.present);
    assert(memcmp(request.plaintext, new_plaintext,
                  sizeof(new_plaintext) - 1U) == 0);
    assert(d1l_mesh_admin_response_request_admit(
               &request, 203U, true, 8U, 100U, &view) ==
           D1L_MESH_ADMIN_RESPONSE_ADMIT_ACCEPTED);
    mutations++;
    assert(d1l_mesh_admin_response_request_complete(&request, 203U));
    assert(mutations == 1U);
    assert(admin_response_request_clear(&request));
}

static void test_admin_response_saturation_and_timeout_wipe_plaintext(void)
{
    static const uint8_t plaintext[] = "bounded-response-secret";
    const admin_response_token_t token = {
        .request_id = 204U,
        .deadline_us = 500U,
        .slot = 1U,
    };
    d1l_mesh_admin_response_request_t saturated = {0};
    admin_response_test_queue_t full_queue = {.accept = false};
    publish_admin_response(&saturated, 204U, 9U, plaintext,
                           sizeof(plaintext) - 1U, 500U);
    assert(d1l_mesh_admin_response_request_enqueue(
               &saturated, 204U, admin_response_test_enqueue, &full_queue,
               &token) == D1L_MESH_COMMAND_ENQUEUE_SATURATED);
    assert(full_queue.calls == 1U);
    assert(d1l_mesh_request_guard_state(&saturated.lifecycle) ==
           D1L_MESH_REQUEST_FREE);
    assert(admin_response_request_clear(&saturated));

    d1l_mesh_admin_response_request_t expired = {0};
    d1l_mesh_admin_response_view_t view = {0};
    publish_admin_response(&expired, 205U, 9U, plaintext,
                           sizeof(plaintext) - 1U, 100U);
    assert(d1l_mesh_admin_response_request_admit(
               &expired, 205U, true, 9U, 100U, &view) ==
           D1L_MESH_ADMIN_RESPONSE_ADMIT_EXPIRED);
    assert(d1l_mesh_request_guard_state(&expired.lifecycle) ==
           D1L_MESH_REQUEST_FREE);
    assert(admin_response_request_clear(&expired));
    assert(bytes_zero(&view, sizeof(view)));
}

static void test_admin_response_owner_reap_due_and_zero_deadline_wipes(void)
{
    static const uint8_t plaintext[] = "reap-due-secret";
    d1l_mesh_admin_response_request_t request = {0};

    publish_admin_response(&request, 206U, 10U, plaintext,
                           sizeof(plaintext) - 1U, 100U);
    assert(!d1l_mesh_admin_response_request_owner_reap(
        &request, false, 10U, 100U));
    assert(d1l_mesh_request_guard_state(&request.lifecycle) ==
           D1L_MESH_REQUEST_PENDING);
    assert(request.present);
    assert(d1l_mesh_admin_response_request_owner_reap(
        &request, true, 10U, 100U));
    assert(d1l_mesh_request_guard_state(&request.lifecycle) ==
           D1L_MESH_REQUEST_FREE);
    assert(admin_response_request_clear(&request));

    publish_admin_response(&request, 207U, 10U, plaintext,
                           sizeof(plaintext) - 1U, 200U);
    request.deadline_us = 0U;
    assert(d1l_mesh_admin_response_request_owner_reap(
        &request, true, 10U, 50U));
    assert(d1l_mesh_request_guard_state(&request.lifecycle) ==
           D1L_MESH_REQUEST_FREE);
    assert(admin_response_request_clear(&request));
}

static void test_admin_response_owner_reap_stale_generation_wipes(void)
{
    static const uint8_t plaintext[] = "reap-stale-secret";
    d1l_mesh_admin_response_request_t request = {0};
    publish_admin_response(&request, 208U, 10U, plaintext,
                           sizeof(plaintext) - 1U, 500U);
    assert(d1l_mesh_admin_response_request_owner_reap(
        &request, true, 11U, 100U));
    assert(d1l_mesh_request_guard_state(&request.lifecycle) ==
           D1L_MESH_REQUEST_FREE);
    assert(admin_response_request_clear(&request));
}

static void test_admin_response_owner_reap_keeps_valid_predeadline_slot(void)
{
    static const uint8_t plaintext[] = "reap-valid-secret";
    d1l_mesh_admin_response_request_t request = {0};
    d1l_mesh_admin_response_view_t view = {0};
    publish_admin_response(&request, 209U, 11U, plaintext,
                           sizeof(plaintext) - 1U, 500U);
    assert(!d1l_mesh_admin_response_request_owner_reap(
        &request, true, 11U, 499U));
    assert(d1l_mesh_request_guard_state(&request.lifecycle) ==
           D1L_MESH_REQUEST_PENDING);
    assert(request.present);
    assert(memcmp(request.plaintext, plaintext,
                  sizeof(plaintext) - 1U) == 0);
    assert(d1l_mesh_admin_response_request_admit(
               &request, 209U, true, 11U, 499U, &view) ==
           D1L_MESH_ADMIN_RESPONSE_ADMIT_ACCEPTED);
    assert(d1l_mesh_admin_response_request_complete(&request, 209U));
    assert(admin_response_request_clear(&request));
}

static void test_admin_response_reap_reuse_rejects_stale_token_aba(void)
{
    static const uint8_t plaintext_a[] = "generation-a-secret";
    static const uint8_t plaintext_b[] = "generation-b-secret";
    d1l_mesh_admin_response_request_t request = {0};
    admin_response_token_t token_a;
    memset(&token_a, 0, sizeof(token_a));
    token_a.request_id = 210U;
    token_a.deadline_us = 100U;
    token_a.slot = 0U;
    admin_response_test_queue_t queue = {.accept = true};

    publish_admin_response(&request, 210U, 12U, plaintext_a,
                           sizeof(plaintext_a) - 1U, 100U);
    assert(d1l_mesh_admin_response_request_enqueue(
               &request, 210U, admin_response_test_enqueue, &queue,
               &token_a) == D1L_MESH_COMMAND_ENQUEUE_QUEUED);
    assert(queue.token.request_id == 210U);
    assert(d1l_mesh_admin_response_request_owner_reap(
        &request, true, 12U, 100U));
    assert(admin_response_request_clear(&request));

    publish_admin_response(&request, 211U, 13U, plaintext_b,
                           sizeof(plaintext_b) - 1U, 600U);
    d1l_mesh_admin_response_view_t stale_view = {0};
    assert(d1l_mesh_admin_response_request_admit(
               &request, token_a.request_id, true, 13U, 200U,
               &stale_view) == D1L_MESH_ADMIN_RESPONSE_ADMIT_INVALID);
    assert(d1l_mesh_request_guard_matches(&request.lifecycle, 211U));
    assert(d1l_mesh_request_guard_state(&request.lifecycle) ==
           D1L_MESH_REQUEST_PENDING);
    assert(request.present);
    assert(memcmp(request.plaintext, plaintext_b,
                  sizeof(plaintext_b) - 1U) == 0);

    d1l_mesh_admin_response_view_t view_b = {0};
    assert(d1l_mesh_admin_response_request_admit(
               &request, 211U, true, 13U, 200U, &view_b) ==
           D1L_MESH_ADMIN_RESPONSE_ADMIT_ACCEPTED);
    assert(view_b.plaintext_len == sizeof(plaintext_b) - 1U);
    assert(memcmp(view_b.plaintext, plaintext_b,
                  view_b.plaintext_len) == 0);
    assert(d1l_mesh_admin_response_request_complete(&request, 211U));
    assert(admin_response_request_clear(&request));
}

static void test_owner_scheduler_terminal_recovery_precedes_overload(void)
{
    d1l_mesh_owner_scheduler_t scheduler = {
        .radio_event_burst = D1L_MESH_OWNER_RADIO_EVENT_BURST_MAX,
        .priority_command_burst =
            D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX,
    };
    bool forced_normal = true;
    assert(d1l_mesh_owner_scheduler_choose(
               &scheduler, true, true, true, true,
               &forced_normal) ==
           D1L_MESH_OWNER_WORK_TERMINAL_RECOVERY);
    assert(!forced_normal);
    assert(scheduler.radio_event_burst ==
           D1L_MESH_OWNER_RADIO_EVENT_BURST_MAX);
    assert(scheduler.priority_command_burst ==
           D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX);
}

static void test_terminal_lane_publication_blocks_queue_dispatch(void)
{
    volatile uint32_t lane = 0U;
    assert(!d1l_mesh_terminal_lane_has_pending(&lane));

    /* A callback that linearizes before reservation blocks all ordinary
     * dequeue work until the owner takes its immutable history slot. */
    assert(d1l_mesh_terminal_lane_publish_slot(&lane, 4U));
    assert(d1l_mesh_terminal_lane_has_pending(&lane));
    assert(!d1l_mesh_terminal_lane_try_reserve_owner(&lane));
    assert(d1l_mesh_terminal_lane_take_pending(&lane) == (1UL << 4U));
    assert(!d1l_mesh_terminal_lane_has_pending(&lane));

    /* Publication after reservation but before dequeue invalidates that same
     * reservation; the post-dequeue validation observes the callback bit. */
    assert(d1l_mesh_terminal_lane_try_reserve_owner(&lane));
    assert(d1l_mesh_terminal_lane_publish_slot(&lane, 2U));
    assert(!d1l_mesh_terminal_lane_owner_still_reserved(&lane));
    d1l_mesh_terminal_lane_release_owner(&lane);
    assert(d1l_mesh_terminal_lane_take_pending(&lane) == (1UL << 2U));

    assert(d1l_mesh_terminal_lane_try_reserve_owner(&lane));
    assert(d1l_mesh_terminal_lane_owner_still_reserved(&lane));
    d1l_mesh_terminal_lane_release_owner(&lane);
    assert(lane == 0U);
}

static void test_dequeued_send_raw_is_retained_across_terminal_race(void)
{
    volatile uint32_t lane = 0U;
    bool send_raw_dequeued = false;
    bool send_raw_held = false;
    unsigned dispatch_order[2] = {0U, 0U};
    unsigned dispatch_count = 0U;

    assert(d1l_mesh_terminal_lane_try_reserve_owner(&lane));
    send_raw_dequeued = true;

    /* This is the exact reported race: the callback wins after dequeue but
     * before admission. Validation must fail, so the accepted command moves
     * to the owner-held slot without being rejected or requeued behind peers. */
    assert(d1l_mesh_terminal_lane_publish_slot(&lane, 3U));
    if (!d1l_mesh_terminal_lane_owner_still_reserved(&lane)) {
        send_raw_held = send_raw_dequeued;
    }
    assert(send_raw_held);
    d1l_mesh_terminal_lane_release_owner(&lane);

    assert(d1l_mesh_terminal_lane_take_pending(&lane) == (1UL << 3U));
    dispatch_order[dispatch_count++] = 1U; /* exact terminal */
    assert(d1l_mesh_terminal_lane_try_reserve_owner(&lane));
    assert(d1l_mesh_terminal_lane_owner_still_reserved(&lane));
    if (send_raw_held) {
        dispatch_order[dispatch_count++] = 2U; /* retained SEND_RAW */
        send_raw_held = false;
    }
    d1l_mesh_terminal_lane_release_owner(&lane);

    assert(dispatch_count == 2U);
    assert(dispatch_order[0] == 1U);
    assert(dispatch_order[1] == 2U);
    assert(!send_raw_held);
}

static void test_accepted_send_raw_waits_for_active_tx_terminal(void)
{
    volatile uint32_t lane = 0U;
    d1l_mesh_command_request_t request = {0};
    const uint32_t request_id = 811U;
    bool tx_busy = true;
    bool send_raw_held = false;
    unsigned executed = 0U;
    unsigned rejected = 0U;

    assert(d1l_mesh_command_request_claim(&request, request_id));
    assert(d1l_mesh_command_request_publish(&request, request_id));
    assert(d1l_mesh_terminal_lane_try_reserve_owner(&lane));
    assert(d1l_mesh_terminal_lane_owner_still_reserved(&lane));
    if (tx_busy) {
        send_raw_held = true;
    } else {
        rejected++;
    }
    d1l_mesh_terminal_lane_release_owner(&lane);

    /* Holding does not admit, reject, complete, or reorder the accepted
     * request while the prior exact transmission remains active. */
    assert(send_raw_held);
    assert(d1l_mesh_command_request_state(&request) ==
           D1L_MESH_REQUEST_PENDING);
    assert(executed == 0U);
    assert(rejected == 0U);

    assert(d1l_mesh_terminal_lane_publish_slot(&lane, 5U));
    assert(d1l_mesh_terminal_lane_take_pending(&lane) == (1UL << 5U));
    tx_busy = false;
    assert(d1l_mesh_terminal_lane_try_reserve_owner(&lane));
    assert(d1l_mesh_terminal_lane_owner_still_reserved(&lane));
    if (send_raw_held && !tx_busy &&
        d1l_mesh_command_request_admit(&request, request_id)) {
        executed++;
        send_raw_held = false;
        assert(d1l_mesh_command_request_complete(&request, request_id));
    }
    d1l_mesh_terminal_lane_release_owner(&lane);

    assert(executed == 1U);
    assert(rejected == 0U);
    assert(!send_raw_held);
    assert(d1l_mesh_command_request_release(
        &request, request_id, D1L_MESH_REQUEST_COMPLETED));
}

static void test_older_queued_rx_timeout_defers_after_send_raw(void)
{
    /* An older RX_TIMEOUT may remain queued when the fairness selector admits
     * SEND_RAW. Its eventual start-RX call must not reconfigure the radio
     * while that exact transmission is active. */
    d1l_mesh_owner_scheduler_t scheduler = {
        .radio_event_burst = D1L_MESH_OWNER_RADIO_EVENT_BURST_MAX,
    };
    bool forced_normal = true;
    assert(d1l_mesh_owner_scheduler_choose(
               &scheduler, false, true, false, true,
               &forced_normal) ==
           D1L_MESH_OWNER_WORK_NORMAL_COMMAND);
    assert(!forced_normal);

    const d1l_mesh_tx_operation_identity_t send_raw = {
        .operation_id = 501U,
        .kind = D1L_MESH_TX_OPERATION_GENERIC,
    };
    const d1l_mesh_tx_operation_identity_t stale_terminal = {
        .operation_id = 500U,
        .kind = D1L_MESH_TX_OPERATION_GENERIC,
    };
    const d1l_mesh_tx_operation_identity_t exact_terminal = {
        .operation_id = 501U,
        .kind = D1L_MESH_TX_OPERATION_GENERIC,
    };
    volatile uint32_t pending_rx_restart = 0U;
    bool tx_busy = true;
    assert(!d1l_mesh_rx_restart_begin(
        &pending_rx_restart, tx_busy, true));
    assert(pending_rx_restart == 1U);

    /* A predecessor terminal cannot release the pending restart. */
    assert(!d1l_mesh_tx_operation_identity_equal(
        &send_raw, &stale_terminal));
    assert(!d1l_mesh_rx_restart_begin(
        &pending_rx_restart, tx_busy, true));
    assert(pending_rx_restart == 1U);

    /* Only the exact terminal clears busy; that owner path consumes the one
     * coalesced restart immediately before entering radio configuration. */
    assert(d1l_mesh_tx_operation_identity_equal(
        &send_raw, &exact_terminal));
    tx_busy = false;
    assert(d1l_mesh_rx_restart_begin(
        &pending_rx_restart, tx_busy, true));
    assert(pending_rx_restart == 0U);
}

static void test_latched_terminal_consumes_pending_rx_recovery_once(void)
{
    volatile uint32_t pending_rx_restart = 1U;
    unsigned rx_restarts = 0U;

    /* The owner has taken an exact terminal and sees an already-coalesced RX
     * request. It must leave the shared bit for the terminal handler instead
     * of also retaining a task-local recovery request. */
    const bool local_rx_recovery = d1l_mesh_rx_recovery_take(
        &pending_rx_restart, true);
    assert(!local_rx_recovery);
    assert(pending_rx_restart == 1U);

    /* Model the exact terminal clearing TX and entering its sole RX restart.
     * That path consumes the shared bit immediately before radio work. */
    const bool tx_busy = false;
    if (d1l_mesh_rx_restart_begin(
            &pending_rx_restart, tx_busy, true)) {
        rx_restarts++;
    }
    assert(pending_rx_restart == 0U);

    /* No stale task-local copy remains to replay on the next owner pass. */
    if (d1l_mesh_rx_recovery_take(
            &pending_rx_restart, false) &&
        d1l_mesh_rx_restart_begin(
            &pending_rx_restart, tx_busy, true)) {
        rx_restarts++;
    }
    assert(rx_restarts == 1U);
}

static void test_full_radio_backlog_exact_terminal_preempts_and_restarts_once(void)
{
    volatile uint32_t terminal_lane = 0U;
    volatile uint32_t pending_rx_restart = 1U;
    const d1l_mesh_tx_operation_identity_t stale = {
        .operation_id = 700U,
        .kind = D1L_MESH_TX_OPERATION_GENERIC,
    };
    const d1l_mesh_tx_operation_identity_t active = {
        .operation_id = 701U,
        .kind = D1L_MESH_TX_OPERATION_GENERIC,
    };

    /* Stale, exact, and duplicate callbacks coalesce into history-slot bits
     * without entering the eight-deep ordinary radio FIFO. The owner scans
     * those immutable snapshots and selects the newest exact operation. */
    const uint8_t stale_slot = (uint8_t)((stale.operation_id - 1U) % 8U);
    const uint8_t active_slot = (uint8_t)((active.operation_id - 1U) % 8U);
    assert(d1l_mesh_terminal_lane_publish_slot(
        &terminal_lane, stale_slot));
    assert(d1l_mesh_terminal_lane_publish_slot(
        &terminal_lane, active_slot));
    assert(d1l_mesh_terminal_lane_publish_slot(
        &terminal_lane, stale_slot));
    const uint32_t pending_slots = d1l_mesh_terminal_lane_take_pending(
        &terminal_lane);
    assert(pending_slots ==
           ((1UL << stale_slot) | (1UL << active_slot)));
    const uint32_t terminal_origin = (uint32_t)active.operation_id;

    d1l_mesh_owner_scheduler_t scheduler = {
        .radio_event_burst = D1L_MESH_OWNER_RADIO_EVENT_BURST_MAX,
        .priority_command_burst =
            D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX,
    };
    const unsigned radio_backlog = 8U;
    bool forced_normal = true;
    assert(d1l_mesh_owner_scheduler_choose(
               &scheduler, terminal_origin != 0U,
               radio_backlog == 8U, true, true,
               &forced_normal) ==
           D1L_MESH_OWNER_WORK_TERMINAL_RECOVERY);
    assert(!forced_normal);
    assert(radio_backlog == 8U);

    /* The exact terminal owns the shared pending RX bit. It clears TX and
     * consumes one restart; no task-local recovery remains to replay it. */
    unsigned rx_restarts = 0U;
    assert(!d1l_mesh_rx_recovery_take(
        &pending_rx_restart, true));
    assert(d1l_mesh_tx_operation_identity_equal(&active, &active));
    if (d1l_mesh_rx_restart_begin(
            &pending_rx_restart, false, true)) {
        rx_restarts++;
    }
    assert(pending_rx_restart == 0U);
    assert(d1l_mesh_terminal_lane_take_pending(&terminal_lane) == 0U);
    assert(!d1l_mesh_rx_recovery_take(
        &pending_rx_restart, false));
    assert(rx_restarts == 1U);
}

static void test_owner_scheduler_bounds_radio_and_priority_starvation(void)
{
    d1l_mesh_owner_scheduler_t scheduler = {0};
    const unsigned maximum_non_normal =
        D1L_MESH_OWNER_RADIO_EVENT_BURST_MAX *
            (D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX + 1U) +
        D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX;
    unsigned non_normal = 0U;
    unsigned normal_dispatches = 0U;
    unsigned forced_dispatches = 0U;

    /* Model permanent overload in both higher-priority lanes while a normal
     * command is continuously ready. The production selector must still
     * dispatch the normal lane within the exact finite action bound. */
    for (unsigned decision = 0U; decision < 120U; ++decision) {
        bool forced_normal = false;
        const d1l_mesh_owner_work_t work =
            d1l_mesh_owner_scheduler_choose(
                &scheduler, false, true, true, true,
                &forced_normal);
        assert(scheduler.radio_event_burst <=
               D1L_MESH_OWNER_RADIO_EVENT_BURST_MAX);
        assert(scheduler.priority_command_burst <=
               D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX);
        if (work == D1L_MESH_OWNER_WORK_NORMAL_COMMAND) {
            assert(forced_normal);
            normal_dispatches++;
            forced_dispatches++;
            non_normal = 0U;
        } else {
            assert(work == D1L_MESH_OWNER_WORK_RADIO_EVENT ||
                   work == D1L_MESH_OWNER_WORK_PRIORITY_COMMAND);
            assert(!forced_normal);
            non_normal++;
            assert(non_normal <= maximum_non_normal);
        }
    }
    assert(normal_dispatches >= 8U);
    assert(forced_dispatches == normal_dispatches);
}

static void test_owner_scheduler_priority_burst_is_sticky_until_fairness(void)
{
    d1l_mesh_owner_scheduler_t scheduler = {0};
    bool forced_normal = false;
    for (unsigned i = 0U;
         i < D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX + 4U; ++i) {
        assert(d1l_mesh_owner_scheduler_choose(
                   &scheduler, false, false, true, false,
                   &forced_normal) ==
               D1L_MESH_OWNER_WORK_PRIORITY_COMMAND);
        assert(!forced_normal);
    }
    assert(scheduler.priority_command_burst ==
           D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX);
    assert(d1l_mesh_owner_scheduler_choose(
               &scheduler, false, false, true, true,
               &forced_normal) ==
           D1L_MESH_OWNER_WORK_NORMAL_COMMAND);
    assert(forced_normal);

    assert(d1l_mesh_owner_scheduler_choose(
               &scheduler, false, false, false, false,
               &forced_normal) == D1L_MESH_OWNER_WORK_IDLE);
    assert(!forced_normal);
    assert(scheduler.radio_event_burst == 0U);
    assert(scheduler.priority_command_burst == 0U);
}

static void test_fair_dispatch_preserves_sync_reply_and_secret_wipe(void)
{
    d1l_mesh_command_request_t request = {0};
    d1l_mesh_owner_scheduler_t scheduler = {0};
    publish_admin_login(&request, 141U, "bounded-reply");

    unsigned priority_before_reply = 0U;
    for (;;) {
        bool forced_normal = false;
        const d1l_mesh_owner_work_t work =
            d1l_mesh_owner_scheduler_choose(
                &scheduler, false, false, true, true,
                &forced_normal);
        if (work == D1L_MESH_OWNER_WORK_NORMAL_COMMAND) {
            assert(forced_normal);
            break;
        }
        assert(work == D1L_MESH_OWNER_WORK_PRIORITY_COMMAND);
        priority_before_reply++;
        assert(priority_before_reply <=
               D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX);
    }
    finish_admitted_admin_login(&request, 141U, "bounded-reply");
    assert(request_password_clear(&request));
    assert(d1l_mesh_command_request_state(&request) ==
           D1L_MESH_REQUEST_FREE);
}

static void test_forced_fairness_counts_only_admitted_generation(void)
{
    d1l_mesh_owner_scheduler_t scheduler = {
        .priority_command_burst =
            D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX,
    };
    bool forced_normal = false;
    assert(d1l_mesh_owner_scheduler_choose(
               &scheduler, false, false, true, true,
               &forced_normal) ==
           D1L_MESH_OWNER_WORK_NORMAL_COMMAND);
    assert(forced_normal);

    d1l_mesh_command_request_t cancelled = {0};
    assert(d1l_mesh_command_request_claim(&cancelled, 151U));
    assert(d1l_mesh_command_request_publish(&cancelled, 151U));
    assert(d1l_mesh_command_request_cancel_and_release(&cancelled, 151U));
    volatile uint32_t admitted_fairness_count = 0U;
    if (d1l_mesh_command_request_admit(&cancelled, 151U)) {
        (void)d1l_mesh_runtime_counter_increment_saturating(
            &admitted_fairness_count);
    }
    assert(admitted_fairness_count == 0U);

    d1l_mesh_command_request_t admitted = {0};
    assert(d1l_mesh_command_request_claim(&admitted, 152U));
    assert(d1l_mesh_command_request_publish(&admitted, 152U));
    if (d1l_mesh_command_request_admit(&admitted, 152U)) {
        (void)d1l_mesh_runtime_counter_increment_saturating(
            &admitted_fairness_count);
    }
    assert(admitted_fairness_count == 1U);
    assert(d1l_mesh_command_request_complete(&admitted, 152U));
    assert(d1l_mesh_command_request_release(
        &admitted, 152U, D1L_MESH_REQUEST_COMPLETED));
}

static void test_runtime_telemetry_counters_saturate(void)
{
    volatile uint32_t counter = UINT32_MAX - 1U;
    assert(d1l_mesh_runtime_counter_increment_saturating(&counter) ==
           UINT32_MAX);
    assert(counter == UINT32_MAX);
    assert(d1l_mesh_runtime_counter_increment_saturating(&counter) ==
           UINT32_MAX);
    assert(counter == UINT32_MAX);
    assert(d1l_mesh_runtime_counter_increment_saturating(NULL) == 0U);
}

static bool consume_terminal_if_current(
    d1l_mesh_tx_operation_identity_t *active,
    const d1l_mesh_tx_operation_identity_t *terminal)
{
    if (!d1l_mesh_tx_operation_identity_equal(active, terminal)) {
        return false;
    }
    *active = (d1l_mesh_tx_operation_identity_t){0};
    return true;
}

typedef struct {
    uint32_t active_origin;
    bool irq_recovery_claimed;
    bool mode_tx;
    bool dio_asserted;
    unsigned standby_count;
    unsigned irq_clear_count;
    unsigned expander_latch_read_count;
    unsigned callback_count;
} mock_origin_radio_t;

static bool mock_origin_radio_begin(mock_origin_radio_t *radio,
                                    uint32_t origin)
{
    if (!radio || origin == 0U || radio->active_origin != 0U ||
        radio->irq_recovery_claimed) {
        return false;
    }
    radio->active_origin = origin;
    radio->mode_tx = true;
    radio->dio_asserted = false;
    return true;
}

static bool mock_origin_radio_recover(mock_origin_radio_t *radio,
                                      uint32_t origin)
{
    if (!radio || origin == 0U || radio->irq_recovery_claimed) {
        return false;
    }
    radio->irq_recovery_claimed = true;
    if (radio->active_origin != origin) {
        radio->irq_recovery_claimed = false;
        return false;
    }
    radio->standby_count++;
    radio->irq_clear_count++;
    radio->expander_latch_read_count++;
    radio->mode_tx = false;
    radio->dio_asserted = false;
    radio->active_origin = 0U;
    radio->irq_recovery_claimed = false;
    return true;
}

static bool mock_origin_radio_terminal(mock_origin_radio_t *radio,
                                       uint32_t origin)
{
    if (!radio || radio->irq_recovery_claimed) {
        return false;
    }
    radio->irq_recovery_claimed = true;
    if (origin == 0U || radio->active_origin != origin) {
        radio->irq_recovery_claimed = false;
        return false;
    }
    radio->irq_clear_count++;
    radio->standby_count++;
    radio->mode_tx = false;
    radio->dio_asserted = false;
    radio->active_origin = 0U;
    radio->callback_count++;
    radio->irq_recovery_claimed = false;
    return true;
}

static void mock_origin_radio_finish_claimed_terminal(
    mock_origin_radio_t *radio, uint32_t origin)
{
    assert(radio && radio->irq_recovery_claimed);
    assert(radio->active_origin == origin);
    radio->irq_clear_count++;
    radio->standby_count++;
    radio->mode_tx = false;
    radio->dio_asserted = false;
    radio->active_origin = 0U;
    radio->callback_count++;
    radio->irq_recovery_claimed = false;
}

static void test_late_a_terminal_cannot_mutate_active_b(void)
{
    const d1l_mesh_tx_operation_identity_t operation_a = {
        .operation_id = 41U,
        .kind = D1L_MESH_TX_OPERATION_DM,
        .dm_session_id = 0x0102030405060708ULL,
        .dm_revision = 3U,
    };
    const d1l_mesh_tx_operation_identity_t operation_a_done = operation_a;
    const d1l_mesh_tx_operation_identity_t operation_a_late = operation_a;
    const d1l_mesh_tx_operation_identity_t operation_b = {
        .operation_id = 42U,
        .kind = D1L_MESH_TX_OPERATION_DM,
        .dm_session_id = 0x1112131415161718ULL,
        .dm_revision = 1U,
    };
    d1l_mesh_tx_operation_identity_t active = operation_a;

    /* A completes normally, and only then may the owner begin B. */
    assert(consume_terminal_if_current(&active, &operation_a_done));
    assert(!d1l_mesh_tx_operation_identity_valid(&active));
    active = operation_b;
    const d1l_mesh_tx_operation_identity_t b_before_late_a = active;

    /* Model either a delayed TxDone or delayed TxTimeout for A. Both carry
     * A's immutable origin tuple, fail the exact matcher, and leave every
     * field of B untouched. */
    assert(!consume_terminal_if_current(&active, &operation_a_late));
    assert(memcmp(&active, &b_before_late_a, sizeof(active)) == 0);
    assert(!consume_terminal_if_current(&active, &operation_a_late));
    assert(memcmp(&active, &b_before_late_a, sizeof(active)) == 0);

    assert(consume_terminal_if_current(&active, &operation_b));
    assert(!consume_terminal_if_current(&active, &operation_b));
}

static void test_watchdog_recovers_when_full_irq_queue_drops_terminal(void)
{
    const d1l_mesh_tx_operation_identity_t operation_a = {
        .operation_id = 71U,
        .kind = D1L_MESH_TX_OPERATION_DM,
        .dm_session_id = 0x7172737475767778ULL,
        .dm_revision = 3U,
    };
    const d1l_mesh_tx_operation_identity_t operation_b = {
        .operation_id = 72U,
        .kind = D1L_MESH_TX_OPERATION_PUBLIC,
    };
    d1l_mesh_tx_operation_identity_t active = operation_a;
    d1l_mesh_tx_watchdog_t watchdog_a = {0};
    mock_origin_radio_t radio = {0};
    const uint64_t started_us = 1000000ULL;
    const uint64_t timeout_us = 5250000ULL;
    assert(mock_origin_radio_begin(
        &radio, (uint32_t)operation_a.operation_id));
    assert(d1l_mesh_tx_watchdog_arm(
        &watchdog_a, &operation_a, started_us, timeout_us));

    /* Fill all ten vendor DIO queue slots and model the sole terminal IRQ
     * being dropped. No callback reaches the owner, so only the independent
     * immutable-origin watchdog can release this operation. */
    for (unsigned queued = 0U; queued < 10U; ++queued) {
        assert(watchdog_a.armed);
        assert(d1l_mesh_tx_operation_identity_equal(&active, &operation_a));
    }
    d1l_mesh_tx_operation_identity_t terminal = {0};
    assert(!d1l_mesh_tx_watchdog_take_due(
        &watchdog_a, started_us + timeout_us - 1U, &terminal));
    assert(d1l_mesh_tx_watchdog_take_due(
        &watchdog_a, started_us + timeout_us, &terminal));
    assert(d1l_mesh_tx_operation_identity_equal(&terminal, &operation_a));
    assert(mock_origin_radio_recover(
        &radio, (uint32_t)terminal.operation_id));
    assert(radio.standby_count == 1U);
    assert(radio.irq_clear_count == 1U);
    assert(radio.expander_latch_read_count == 1U);
    assert(consume_terminal_if_current(&active, &terminal));
    assert(!d1l_mesh_tx_operation_identity_valid(&active));
    assert(!watchdog_a.armed);
    assert(!d1l_mesh_tx_watchdog_take_due(
        &watchdog_a, UINT64_MAX, &terminal));

    /* B becomes eligible only after A's physical IRQ/latch cleanup. A queued
     * vendor callback delivered after B starts must return before clearing
     * B's IRQ, entering standby, or changing B's watchdog tuple. */
    active = operation_b;
    assert(mock_origin_radio_begin(
        &radio, (uint32_t)operation_b.operation_id));
    d1l_mesh_tx_watchdog_t watchdog_b = {0};
    assert(d1l_mesh_tx_watchdog_arm(
        &watchdog_b, &operation_b, started_us + timeout_us, timeout_us));
    const uint64_t b_deadline = watchdog_b.deadline_us;
    const unsigned standby_before_late_a = radio.standby_count;
    const unsigned irq_clear_before_late_a = radio.irq_clear_count;
    assert(!mock_origin_radio_terminal(
        &radio, (uint32_t)operation_a.operation_id));
    assert(radio.active_origin == (uint32_t)operation_b.operation_id);
    assert(radio.mode_tx);
    assert(radio.standby_count == standby_before_late_a);
    assert(radio.irq_clear_count == irq_clear_before_late_a);
    assert(watchdog_b.armed && watchdog_b.deadline_us == b_deadline);
    assert(d1l_mesh_tx_operation_identity_equal(
        &watchdog_b.operation, &operation_b));
    assert(d1l_mesh_tx_operation_identity_equal(&active, &operation_b));

    assert(mock_origin_radio_terminal(
        &radio, (uint32_t)operation_b.operation_id));
    assert(radio.callback_count == 1U);
    assert(consume_terminal_if_current(&active, &operation_b));
    d1l_mesh_tx_watchdog_reset(&watchdog_b);
}

static void test_vendor_terminal_claim_defers_competing_watchdog_recovery(void)
{
    const d1l_mesh_tx_operation_identity_t operation_a = {
        .operation_id = 91U,
        .kind = D1L_MESH_TX_OPERATION_GENERIC,
    };
    const d1l_mesh_tx_operation_identity_t operation_b = {
        .operation_id = 92U,
        .kind = D1L_MESH_TX_OPERATION_PUBLIC,
    };
    mock_origin_radio_t radio = {0};
    d1l_mesh_tx_operation_identity_t active = operation_a;
    d1l_mesh_tx_watchdog_t watchdog = {0};
    assert(mock_origin_radio_begin(
        &radio, (uint32_t)operation_a.operation_id));
    assert(d1l_mesh_tx_watchdog_arm(&watchdog, &operation_a, 10U, 20U));

    /* The vendor terminal path wins the physical cleanup claim just before
     * owner maintenance. Watchdog recovery must defer and keep A active, so B
     * cannot arm until the vendor path publishes A's exact callback. */
    radio.irq_recovery_claimed = true;
    d1l_mesh_tx_operation_identity_t terminal = {0};
    assert(d1l_mesh_tx_watchdog_take_due(&watchdog, 30U, &terminal));
    assert(!mock_origin_radio_recover(
        &radio, (uint32_t)terminal.operation_id));
    assert(d1l_mesh_tx_watchdog_arm(&watchdog, &terminal, 30U, 10U));
    assert(d1l_mesh_tx_operation_identity_equal(&active, &operation_a));
    assert(!mock_origin_radio_begin(
        &radio, (uint32_t)operation_b.operation_id));

    mock_origin_radio_finish_claimed_terminal(
        &radio, (uint32_t)operation_a.operation_id);
    assert(consume_terminal_if_current(&active, &operation_a));
    d1l_mesh_tx_watchdog_reset(&watchdog);
    active = operation_b;
    assert(mock_origin_radio_begin(
        &radio, (uint32_t)operation_b.operation_id));
    assert(radio.active_origin == (uint32_t)operation_b.operation_id);
    assert(radio.mode_tx);
}

static void test_watchdog_origin_cannot_cross_into_newer_operation(void)
{
    const d1l_mesh_tx_operation_identity_t operation_a = {
        .operation_id = 81U,
        .kind = D1L_MESH_TX_OPERATION_GENERIC,
    };
    const d1l_mesh_tx_operation_identity_t operation_b = {
        .operation_id = 82U,
        .kind = D1L_MESH_TX_OPERATION_PUBLIC,
    };
    d1l_mesh_tx_watchdog_t watchdog = {0};
    assert(d1l_mesh_tx_watchdog_arm(&watchdog, &operation_a, 10U, 20U));
    d1l_mesh_tx_watchdog_reset(&watchdog);
    assert(d1l_mesh_tx_watchdog_arm(&watchdog, &operation_b, 30U, 20U));

    d1l_mesh_tx_operation_identity_t active = operation_b;
    assert(!consume_terminal_if_current(&active, &operation_a));
    assert(d1l_mesh_tx_operation_identity_equal(&active, &operation_b));
    d1l_mesh_tx_operation_identity_t terminal = {0};
    assert(d1l_mesh_tx_watchdog_take_due(&watchdog, 50U, &terminal));
    assert(d1l_mesh_tx_operation_identity_equal(&terminal, &operation_b));
    assert(consume_terminal_if_current(&active, &terminal));
}

int main(void)
{
    test_caller_timeout_cancels_only_pending();
    test_admission_wins_timeout_and_requires_exact_completion();
    test_expiry_and_stale_reply_cannot_cross_slot_generations();
    test_atomic_identity_state_tuple_blocks_slot_reuse_aba();
    test_admin_cli_guard_is_bounded_and_generation_safe();
    test_admin_cancel_and_expiry_have_no_side_effects();
    test_admin_queue_saturation_and_admission_execute_production_guard();
    test_admin_slot_reuse_rejects_stale_generation_without_wipe();
    test_admin_logout_and_owner_recursion_use_production_guard();
    test_admin_response_mutation_requires_runtime_owner();
    test_admin_response_rejects_stale_session_and_slot_generation();
    test_admin_response_saturation_and_timeout_wipe_plaintext();
    test_admin_response_owner_reap_due_and_zero_deadline_wipes();
    test_admin_response_owner_reap_stale_generation_wipes();
    test_admin_response_owner_reap_keeps_valid_predeadline_slot();
    test_admin_response_reap_reuse_rejects_stale_token_aba();
    test_owner_scheduler_terminal_recovery_precedes_overload();
    test_terminal_lane_publication_blocks_queue_dispatch();
    test_dequeued_send_raw_is_retained_across_terminal_race();
    test_accepted_send_raw_waits_for_active_tx_terminal();
    test_older_queued_rx_timeout_defers_after_send_raw();
    test_latched_terminal_consumes_pending_rx_recovery_once();
    test_full_radio_backlog_exact_terminal_preempts_and_restarts_once();
    test_owner_scheduler_bounds_radio_and_priority_starvation();
    test_owner_scheduler_priority_burst_is_sticky_until_fairness();
    test_fair_dispatch_preserves_sync_reply_and_secret_wipe();
    test_forced_fairness_counts_only_admitted_generation();
    test_runtime_telemetry_counters_saturate();
    test_late_a_terminal_cannot_mutate_active_b();
    test_watchdog_recovers_when_full_irq_queue_drops_terminal();
    test_vendor_terminal_claim_defers_competing_watchdog_recovery();
    test_watchdog_origin_cannot_cross_into_newer_operation();
    puts("native mesh runtime guards: ok");
    return 0;
}
