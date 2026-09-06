#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mesh/meshcore_ack_dispatch.h"
#include "mesh/meshcore_dm_retry.h"
#include "mesh/meshcore_path_dispatch.h"
#include "mesh/meshcore_route_selection.h"

static void test_valid_direct_path_selection(void)
{
    uint8_t learned_path[] = {0x10U, 0x11U, 0x20U, 0x21U};
    d1l_meshcore_route_selection_t selection = {0};
    const uint32_t learned_at = 1000U;

    assert(d1l_meshcore_route_select(
        true, true, learned_path, 0x42U, learned_at,
        learned_at + D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS,
        1U, &selection));
    assert(selection.route == D1L_MESHCORE_ROUTE_DIRECT);
    assert(selection.reason == D1L_MESHCORE_ROUTE_SELECTION_DIRECT_PROVEN);
    assert(selection.path_len == 0x42U);
    assert(selection.path_byte_len == sizeof(learned_path));
    assert(selection.path_hash_bytes == 2U);
    assert(selection.path_hops == 2U);
    assert(selection.path_age_ms == D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS);
    assert(memcmp(selection.path, learned_path, sizeof(learned_path)) == 0);

    learned_path[0] = 0xFFU;
    assert(selection.path[0] == 0x10U);

    assert(d1l_meshcore_route_select(
        true, true, NULL, 0U, 2000U, 2000U, 2U, &selection));
    assert(selection.route == D1L_MESHCORE_ROUTE_DIRECT);
    assert(selection.path_len == 0U);
    assert(selection.path_byte_len == 0U);
    assert(selection.path_hash_bytes == 1U);
    assert(selection.path_hops == 0U);
}

static void expect_flood(
    const d1l_meshcore_route_selection_t *selection,
    d1l_meshcore_route_selection_reason_t reason,
    uint8_t hash_bytes)
{
    assert(selection->route == D1L_MESHCORE_ROUTE_FLOOD);
    assert(selection->reason == reason);
    assert(selection->path_len == (uint8_t)((hash_bytes - 1U) << 6U));
    assert(selection->path_byte_len == 0U);
    assert(selection->path_hash_bytes == hash_bytes);
    assert(selection->path_hops == 0U);
}

static void test_missing_stale_and_malformed_fallback(void)
{
    const uint8_t one_hop[] = {0x33U};
    d1l_meshcore_route_selection_t selection = {0};

    assert(d1l_meshcore_route_select(
        false, false, one_hop, 0x01U, 0U, 100U, 3U, &selection));
    expect_flood(&selection, D1L_MESHCORE_ROUTE_SELECTION_FLOOD_NO_PATH, 3U);
    assert(selection.path_age_ms == 0U);

    assert(d1l_meshcore_route_select(
        true, true, one_hop, 0x01U, 100U,
        101U + D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS,
        2U, &selection));
    expect_flood(&selection, D1L_MESHCORE_ROUTE_SELECTION_FLOOD_STALE_PATH, 2U);
    assert(selection.path_age_ms == D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS + 1U);

    assert(d1l_meshcore_route_select(
        true, true, one_hop, 0xC0U, 100U, 101U, 1U, &selection));
    expect_flood(&selection, D1L_MESHCORE_ROUTE_SELECTION_FLOOD_MALFORMED_PATH, 1U);

    assert(d1l_meshcore_route_select(
        true, true, NULL, 0x01U, 100U, 101U, 1U, &selection));
    expect_flood(&selection, D1L_MESHCORE_ROUTE_SELECTION_FLOOD_MALFORMED_PATH, 1U);

    assert(d1l_meshcore_route_select(
        true, true, one_hop, 0x01U, 500U, 100U, 1U, &selection));
    expect_flood(&selection, D1L_MESHCORE_ROUTE_SELECTION_FLOOD_STALE_PATH, 1U);
    assert(selection.path_age_ms == UINT32_MAX - 399U);
}

static void test_preboot_timestamp_alias_never_becomes_fresh(void)
{
    const uint8_t one_hop[] = {0x55U};
    d1l_meshcore_route_selection_t selection = {0};

    /* The persisted uptime exactly aliases the later boot's current uptime. */
    assert(d1l_meshcore_route_select(
        true, false, one_hop, 0x01U, 500U, 500U, 1U, &selection));
    expect_flood(
        &selection, D1L_MESHCORE_ROUTE_SELECTION_FLOOD_PREBOOT_PATH, 1U);
    assert(selection.path_age_ms == 0U);

    assert(d1l_meshcore_route_select(
        true, true, one_hop, 0x01U, 500U, 500U, 1U, &selection));
    assert(selection.route == D1L_MESHCORE_ROUTE_DIRECT);
    assert(selection.reason == D1L_MESHCORE_ROUTE_SELECTION_DIRECT_PROVEN);
}

static d1l_meshcore_wire_packet_t direct_inbound_packet(void)
{
    d1l_meshcore_wire_packet_t packet = {0};
    packet.header = (uint8_t)((D1L_MESHCORE_PAYLOAD_TEXT << 2U) |
                              D1L_MESHCORE_ROUTE_DIRECT);
    packet.route = D1L_MESHCORE_ROUTE_DIRECT;
    packet.type = D1L_MESHCORE_PAYLOAD_TEXT;
    packet.version = D1L_MESHCORE_PAYLOAD_VER_1;
    packet.path_len = 0U;
    packet.path_hash_bytes = 1U;
    return packet;
}

static void expect_ack_kind_for_selection(
    const d1l_meshcore_route_selection_t *selection,
    d1l_meshcore_ack_dispatch_kind_t expected_kind)
{
    const d1l_meshcore_wire_packet_t inbound = direct_inbound_packet();
    const uint8_t ack_hash[] = {1U, 2U, 3U, 4U};
    const bool direct = selection->route == D1L_MESHCORE_ROUTE_DIRECT;
    d1l_meshcore_ack_dispatch_plan_t plan = {0};
    assert(d1l_meshcore_ack_dispatch_plan(
        &inbound, 2U, direct, direct ? selection->path : NULL,
        direct ? selection->path_len : 0U, ack_hash, 0U, 0x42U, &plan));
    assert(plan.kind == expected_kind);
    assert(plan.route == (direct ? D1L_MESHCORE_ROUTE_DIRECT :
                                   D1L_MESHCORE_ROUTE_FLOOD));
    if (direct) {
        assert(plan.path_len == selection->path_len);
        assert(plan.path_byte_len == selection->path_byte_len);
        assert(memcmp(plan.path, selection->path, selection->path_byte_len) == 0);
    }
}

static void test_ack_plans_consume_immutable_freshness_selection(void)
{
    uint8_t learned_path[] = {0x66U};
    d1l_meshcore_route_selection_t selection = {0};

    assert(d1l_meshcore_route_select(
        true, true, learned_path, 0x01U, 100U,
        101U + D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS,
        2U, &selection));
    expect_flood(
        &selection, D1L_MESHCORE_ROUTE_SELECTION_FLOOD_STALE_PATH, 2U);
    const d1l_meshcore_route_selection_t stale_snapshot = selection;
    learned_path[0] = 0xFFU;
    assert(memcmp(&selection, &stale_snapshot, sizeof(selection)) == 0);
    expect_ack_kind_for_selection(
        &selection, D1L_MESHCORE_ACK_DISPATCH_FLOOD_ACK);

    assert(d1l_meshcore_route_select(
        true, true, learned_path, 0xC0U, 100U, 101U, 2U, &selection));
    expect_flood(
        &selection, D1L_MESHCORE_ROUTE_SELECTION_FLOOD_MALFORMED_PATH, 2U);
    expect_ack_kind_for_selection(
        &selection, D1L_MESHCORE_ACK_DISPATCH_FLOOD_ACK);

    assert(d1l_meshcore_route_select(
        true, false, learned_path, 0x01U, 101U, 101U, 2U, &selection));
    expect_flood(
        &selection, D1L_MESHCORE_ROUTE_SELECTION_FLOOD_PREBOOT_PATH, 2U);
    expect_ack_kind_for_selection(
        &selection, D1L_MESHCORE_ACK_DISPATCH_FLOOD_ACK);

    learned_path[0] = 0x77U;
    assert(d1l_meshcore_route_select(
        true, true, learned_path, 0x01U, 100U, 101U, 2U, &selection));
    expect_ack_kind_for_selection(
        &selection, D1L_MESHCORE_ACK_DISPATCH_DIRECT_ACK);
    assert(selection.path[0] == 0x77U);

    assert(d1l_meshcore_route_select(
        true, true, NULL, 0U, 100U, 101U, 2U, &selection));
    assert(selection.route == D1L_MESHCORE_ROUTE_DIRECT);
    assert(selection.path_len == 0U);
    expect_ack_kind_for_selection(
        &selection, D1L_MESHCORE_ACK_DISPATCH_DIRECT_ACK);
}

static void test_fail_closed_without_partial_output(void)
{
    const uint8_t path[] = {0x44U};
    d1l_meshcore_route_selection_t selection;
    memset(&selection, 0xA5, sizeof(selection));
    const d1l_meshcore_route_selection_t before = selection;

    assert(!d1l_meshcore_route_select(
        true, true, path, 0x01U, 1U, 2U, 0U, &selection));
    assert(memcmp(&selection, &before, sizeof(selection)) == 0);
    assert(!d1l_meshcore_route_select(
        true, true, path, 0x01U, 1U, 2U, 4U, &selection));
    assert(memcmp(&selection, &before, sizeof(selection)) == 0);
    assert(!d1l_meshcore_route_select(
        true, true, path, 0x01U, 1U, 2U, 1U, NULL));
}

static void test_canonical_retained_lifecycle_and_exact_fallback(void)
{
    const uint8_t path[] = {0x31U};
    d1l_meshcore_path_state_t state = {0};
    assert(d1l_meshcore_path_state_learn(
        &state, D1L_MESHCORE_PATH_SOURCE_ACK_PATH, 1000U));
    const uint32_t generation = state.generation;
    d1l_meshcore_route_selection_t selection = {0};

    /* Retained boot-relative timestamps never authorize a later boot. */
    assert(d1l_meshcore_route_select_canonical(
        true, false, path, 0x01U, &state, 1001U, 2U, &selection));
    expect_flood(
        &selection, D1L_MESHCORE_ROUTE_SELECTION_FLOOD_PREBOOT_PATH, 2U);
    assert(selection.path_generation == generation);

    assert(d1l_meshcore_route_select_canonical(
        true, true, path, 0x01U, &state, 1001U, 2U, &selection));
    assert(selection.route == D1L_MESHCORE_ROUTE_DIRECT);
    assert(selection.path_generation == generation);

    assert(d1l_meshcore_path_state_note_direct_result(
        &state, generation, true, 2000U) ==
        D1L_MESHCORE_PATH_RESULT_UPDATED);
    assert(d1l_meshcore_route_select_canonical(
        true, true, path, 0x01U, &state, 2001U, 2U, &selection));
    assert(selection.route == D1L_MESHCORE_ROUTE_DIRECT);
    assert(selection.path_age_ms == 1U);

    assert(d1l_meshcore_path_state_note_direct_result(
        &state, generation, false, 3000U) ==
        D1L_MESHCORE_PATH_RESULT_UPDATED);
    assert(d1l_meshcore_path_state_note_direct_result(
        &state, generation, false, 4000U) ==
        D1L_MESHCORE_PATH_RESULT_FLOOD_FALLBACK);
    assert(state.lifecycle == D1L_MESHCORE_PATH_STATE_FAILED);
    assert(d1l_meshcore_route_select_canonical(
        false, false, NULL, 0U, &state, 4001U, 2U, &selection));
    expect_flood(
        &selection, D1L_MESHCORE_ROUTE_SELECTION_FLOOD_FAILED_PATH, 2U);

    assert(d1l_meshcore_path_state_learn(
        &state, D1L_MESHCORE_PATH_SOURCE_PATH_RESPONSE, 5000U));
    assert(!d1l_meshcore_path_state_expire_if_due(
        &state, 5000U + D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS));
    assert(d1l_meshcore_path_state_expire_if_due(
        &state, 5001U + D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS));
    assert(d1l_meshcore_route_select_canonical(
        false, false, NULL, 0U, &state,
        5001U + D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS,
        1U, &selection));
    expect_flood(
        &selection, D1L_MESHCORE_ROUTE_SELECTION_FLOOD_EXPIRED_PATH, 1U);
}

static void test_owner_clock_ack_deadline_is_bounded_and_take_once(void)
{
    d1l_meshcore_dm_ack_deadline_t deadline = {0};
    assert(d1l_meshcore_dm_ack_deadline_arm(
        &deadline, 100U, D1L_MESHCORE_ROUTE_DIRECT));
    const uint64_t direct_due =
        100U + (uint64_t)D1L_MESHCORE_DIRECT_ACK_TIMEOUT_MS * 1000ULL;
    assert(d1l_meshcore_dm_ack_deadline_take_due(
               &deadline, direct_due - 1U, D1L_MESHCORE_ROUTE_DIRECT, 0U) ==
           D1L_MESHCORE_DM_ACK_DEADLINE_NONE);
    assert(d1l_meshcore_dm_ack_deadline_take_due(
               &deadline, direct_due, D1L_MESHCORE_ROUTE_DIRECT, 0U) ==
           D1L_MESHCORE_DM_ACK_DEADLINE_RETRY_FLOOD);
    assert(d1l_meshcore_dm_ack_deadline_take_due(
               &deadline, UINT64_MAX, D1L_MESHCORE_ROUTE_DIRECT, 0U) ==
           D1L_MESHCORE_DM_ACK_DEADLINE_NONE);

    assert(d1l_meshcore_dm_ack_deadline_arm(
        &deadline, 200U, D1L_MESHCORE_ROUTE_FLOOD));
    assert(d1l_meshcore_dm_ack_deadline_take_due(
               &deadline, deadline.deadline_us,
               D1L_MESHCORE_ROUTE_FLOOD, 1U) ==
           D1L_MESHCORE_DM_ACK_DEADLINE_FAIL_TIMEOUT);

    assert(d1l_meshcore_dm_ack_deadline_arm(
        &deadline, 300U, D1L_MESHCORE_ROUTE_DIRECT));
    assert(d1l_meshcore_dm_ack_deadline_take_due(
               &deadline, deadline.deadline_us,
               D1L_MESHCORE_ROUTE_DIRECT, 1U) ==
           D1L_MESHCORE_DM_ACK_DEADLINE_FAIL_TIMEOUT);

    assert(d1l_meshcore_dm_ack_deadline_arm(
        &deadline, UINT64_MAX - 10U, D1L_MESHCORE_ROUTE_FLOOD));
    assert(deadline.deadline_us == UINT64_MAX);
    d1l_meshcore_dm_ack_deadline_clear(&deadline);
    assert(!deadline.armed);
    assert(!d1l_meshcore_dm_ack_deadline_arm(&deadline, 0U, 0xffU));
}

static void test_due_ack_deadline_survives_radio_contention_once(void)
{
    d1l_meshcore_dm_ack_deadline_t deadline = {0};
    assert(d1l_meshcore_dm_ack_deadline_arm(
        &deadline, 100U, D1L_MESHCORE_ROUTE_DIRECT));
    const uint64_t due = deadline.deadline_us;

    unsigned flood_attempts = 0U;
    if (d1l_meshcore_dm_ack_deadline_take_due_when_idle(
            &deadline, due, D1L_MESHCORE_ROUTE_DIRECT, 0U, true) ==
        D1L_MESHCORE_DM_ACK_DEADLINE_RETRY_FLOOD) {
        flood_attempts++;
    }
    assert(deadline.armed);
    assert(flood_attempts == 0U);

    if (d1l_meshcore_dm_ack_deadline_take_due_when_idle(
            &deadline, due + 1U, D1L_MESHCORE_ROUTE_DIRECT, 0U, false) ==
        D1L_MESHCORE_DM_ACK_DEADLINE_RETRY_FLOOD) {
        flood_attempts++;
    }
    assert(!deadline.armed);
    assert(flood_attempts == 1U);
    assert(d1l_meshcore_dm_ack_deadline_take_due_when_idle(
               &deadline, UINT64_MAX, D1L_MESHCORE_ROUTE_DIRECT, 0U, false) ==
           D1L_MESHCORE_DM_ACK_DEADLINE_NONE);
    assert(flood_attempts == 1U);
}

static void test_path_generation_saturates_and_success_at_zero_is_explicit(void)
{
    d1l_meshcore_path_state_t state = {0};
    state.generation = UINT32_MAX;
    const d1l_meshcore_path_state_t before = state;
    assert(!d1l_meshcore_path_state_learn(
        &state, D1L_MESHCORE_PATH_SOURCE_OBSERVED, 1U));
    assert(memcmp(&state, &before, sizeof(state)) == 0);

    memset(&state, 0, sizeof(state));
    assert(d1l_meshcore_path_state_learn(
        &state, D1L_MESHCORE_PATH_SOURCE_ACK_PATH, UINT32_MAX));
    const uint32_t generation = state.generation;
    assert(d1l_meshcore_path_state_note_direct_result(
               &state, generation, true, 0U) ==
           D1L_MESHCORE_PATH_RESULT_UPDATED);
    assert((state.flags & D1L_MESHCORE_PATH_FLAG_HAS_SUCCESS) != 0U);
    assert(d1l_meshcore_path_state_validated_at_ms(&state) == 0U);
    assert(!d1l_meshcore_path_state_expire_if_due(
        &state, D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS));
    assert(d1l_meshcore_path_state_expire_if_due(
        &state, D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS + 1U));
}

static void test_path_subtype_vectors_and_correlated_response_surface(void)
{
    const uint8_t ack[] = {0x00U, 0x03U, 1U, 2U, 3U, 4U};
    d1l_meshcore_path_plain_t decoded = {0};
    assert(d1l_meshcore_path_plain_decode(ack, sizeof(ack), &decoded));
    assert(decoded.raw_extra_type == 0x03U);
    assert(decoded.extra_type == D1L_MESHCORE_PAYLOAD_ACK);
    assert(decoded.source == D1L_MESHCORE_PATH_SOURCE_ACK_PATH);
    assert(decoded.kind == D1L_MESHCORE_PATH_EXTRA_ACK);

    uint8_t reserved_ack[sizeof(ack)] = {0};
    memcpy(reserved_ack, ack, sizeof(ack));
    reserved_ack[1] = 0x13U;
    assert(d1l_meshcore_path_plain_decode(
        reserved_ack, sizeof(reserved_ack), &decoded));
    assert(decoded.extra_type == D1L_MESHCORE_PAYLOAD_ACK);
    assert(decoded.source == D1L_MESHCORE_PATH_SOURCE_ACK_PATH);
    assert(decoded.kind == D1L_MESHCORE_PATH_EXTRA_ACK);

    uint8_t response[sizeof(ack)] = {0};
    memcpy(response, ack, sizeof(ack));
    response[1] = 0x01U;
    assert(d1l_meshcore_path_plain_decode(
        response, sizeof(response), &decoded));
    assert(decoded.source == D1L_MESHCORE_PATH_SOURCE_PATH_RESPONSE);
    assert(decoded.kind == D1L_MESHCORE_PATH_EXTRA_RESPONSE);
    response[1] = 0x11U;
    assert(d1l_meshcore_path_plain_decode(
        response, sizeof(response), &decoded));
    assert(decoded.extra_type == D1L_MESHCORE_PATH_EXTRA_TYPE_RESPONSE);
    assert(decoded.kind == D1L_MESHCORE_PATH_EXTRA_RESPONSE);

    const uint8_t dummy[] = {0x00U, 0xffU, 9U, 8U, 7U, 6U};
    assert(d1l_meshcore_path_plain_decode(dummy, sizeof(dummy), &decoded));
    assert(decoded.extra_type == 0x0fU);
    assert(decoded.source == D1L_MESHCORE_PATH_SOURCE_OBSERVED);
    assert(decoded.kind == D1L_MESHCORE_PATH_EXTRA_NONE);

    const uint8_t missing_extra[] = {0x00U};
    const uint8_t truncated_path[] = {0x01U, 0xaaU};
    assert(!d1l_meshcore_path_plain_decode(
        missing_extra, sizeof(missing_extra), &decoded));
    assert(!d1l_meshcore_path_plain_decode(
        truncated_path, sizeof(truncated_path), &decoded));

    d1l_meshcore_path_response_expectation_t expectation = {
        .tag = 0x04030201U,
        .deadline_us = 500U,
        .active = true,
    };
    assert(d1l_meshcore_path_response_take(
               &expectation, true, &ack[2], 4U, 499U) ==
           D1L_MESHCORE_PATH_RESPONSE_MATCHED);
    assert(!expectation.active);
    assert(d1l_meshcore_path_response_take(
               &expectation, true, &ack[2], 4U, 499U) ==
           D1L_MESHCORE_PATH_RESPONSE_UNMATCHED);
    expectation.active = true;
    assert(d1l_meshcore_path_response_take(
               &expectation, false, &ack[2], 4U, 499U) ==
           D1L_MESHCORE_PATH_RESPONSE_UNMATCHED);
    assert(expectation.active);
    assert(d1l_meshcore_path_response_take(
               &expectation, true, &ack[2], 3U, 499U) ==
           D1L_MESHCORE_PATH_RESPONSE_MALFORMED);
    assert(d1l_meshcore_path_response_take(
               &expectation, true, &ack[2], 4U, 501U) ==
           D1L_MESHCORE_PATH_RESPONSE_EXPIRED);
    assert(!expectation.active);

    d1l_meshcore_reciprocal_path_plan_t reciprocal = {0};
    assert(d1l_meshcore_reciprocal_path_take(
        &reciprocal, true, D1L_MESHCORE_ROUTE_FLOOD));
    assert(!d1l_meshcore_reciprocal_path_take(
        &reciprocal, true, D1L_MESHCORE_ROUTE_FLOOD));
    memset(&reciprocal, 0, sizeof(reciprocal));
    assert(!d1l_meshcore_reciprocal_path_take(
        &reciprocal, true, D1L_MESHCORE_ROUTE_DIRECT));
    assert(!d1l_meshcore_reciprocal_path_take(
        &reciprocal, false, D1L_MESHCORE_ROUTE_FLOOD));
    assert(d1l_meshcore_reciprocal_path_take(
        &reciprocal, true, D1L_MESHCORE_ROUTE_TRANSPORT_FLOOD));
}

static void test_authenticated_path_replay_mutates_and_responds_once(void)
{
    d1l_meshcore_path_replay_cache_t cache = {0};
    uint8_t identity[D1L_MESHCORE_PATH_REPLAY_IDENTITY_BYTES] = {0};
    for (size_t i = 0U; i < sizeof(identity); ++i) {
        identity[i] = (uint8_t)(i + 1U);
    }

    unsigned mutations = 0U;
    unsigned reciprocal_responses = 0U;
    for (unsigned delivery = 0U; delivery < 2U; ++delivery) {
        if (d1l_meshcore_path_replay_take(&cache, identity)) {
            mutations++;
            reciprocal_responses++;
        }
    }
    assert(mutations == 1U);
    assert(reciprocal_responses == 1U);

    assert(d1l_meshcore_path_replay_forget(&cache, identity));
    assert(d1l_meshcore_path_replay_take(&cache, identity));
}

int main(void)
{
    d1l_meshcore_path_state_t admin_path = {0};
    d1l_meshcore_route_selection_t admin_route = {0};
    assert(d1l_meshcore_path_state_learn(
        &admin_path, D1L_MESHCORE_PATH_SOURCE_PATH_RESPONSE, 1000U));
    assert(d1l_meshcore_route_select_admin_request(
        true, true, NULL, 0U, &admin_path, 1100U, 1U, &admin_route));
    assert(admin_route.route == D1L_MESHCORE_ROUTE_DIRECT);
    assert(d1l_meshcore_route_select_admin_request(
        true, false, NULL, 0U, &admin_path, 1100U, 1U, &admin_route));
    assert(admin_route.route == D1L_MESHCORE_ROUTE_FLOOD);
    assert(d1l_meshcore_path_state_learn(
        &admin_path, D1L_MESHCORE_PATH_SOURCE_ADVERT, 1000U));
    assert(d1l_meshcore_route_select_admin_request(
        true, true, NULL, 0U, &admin_path, 1100U, 1U, &admin_route));
    assert(admin_route.route == D1L_MESHCORE_ROUTE_FLOOD);
    assert(d1l_meshcore_path_state_learn(
        &admin_path, D1L_MESHCORE_PATH_SOURCE_PATH_RESPONSE, 1000U));
    assert(d1l_meshcore_route_select_admin_request(
        true, true, NULL, 0U, &admin_path,
        1101U + D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS, 1U, &admin_route));
    assert(admin_route.route == D1L_MESHCORE_ROUTE_FLOOD);
    test_valid_direct_path_selection();
    test_missing_stale_and_malformed_fallback();
    test_preboot_timestamp_alias_never_becomes_fresh();
    test_ack_plans_consume_immutable_freshness_selection();
    test_fail_closed_without_partial_output();
    test_canonical_retained_lifecycle_and_exact_fallback();
    test_owner_clock_ack_deadline_is_bounded_and_take_once();
    test_due_ack_deadline_survives_radio_contention_once();
    test_path_generation_saturates_and_success_at_zero_is_explicit();
    test_path_subtype_vectors_and_correlated_response_surface();
    test_authenticated_path_replay_mutates_and_responds_once();
    assert(strcmp(d1l_meshcore_route_selection_reason_name(
                      D1L_MESHCORE_ROUTE_SELECTION_NONE),
                  "none") == 0);
    assert(strcmp(d1l_meshcore_route_selection_reason_name(
                      D1L_MESHCORE_ROUTE_SELECTION_FLOOD_STALE_PATH),
                  "flood_stale_path") == 0);
    assert(strcmp(d1l_meshcore_route_selection_reason_name(
                      D1L_MESHCORE_ROUTE_SELECTION_FLOOD_PREBOOT_PATH),
                  "flood_preboot_path") == 0);
    assert(strcmp(d1l_meshcore_route_selection_reason_name(
                      D1L_MESHCORE_ROUTE_SELECTION_FLOOD_FAILED_PATH),
                  "flood_failed_path") == 0);
    assert(strcmp(d1l_meshcore_route_selection_reason_name(
                      D1L_MESHCORE_ROUTE_SELECTION_FLOOD_DIRECT_RETRY),
                  "flood_direct_retry") == 0);
    puts("meshcore route selection vectors passed");
    return 0;
}
