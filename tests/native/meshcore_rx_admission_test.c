#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mesh/meshcore_rx_admission.h"

static d1l_meshcore_channel_dispatch_outcome_t run_dispatch(
    const bool *authenticated, size_t count, size_t *selected)
{
    d1l_meshcore_channel_dispatch_t dispatch = {0};
    assert(d1l_meshcore_channel_dispatch_begin(&dispatch, count));
    for (size_t index = 0U; index < count; ++index) {
        assert(d1l_meshcore_channel_dispatch_observe(
            &dispatch, index, authenticated[index]));
    }
    return d1l_meshcore_channel_dispatch_finish(&dispatch, selected);
}

static void test_unknown_and_failed_channel_are_closed(void)
{
    size_t selected = 99U;
    assert(run_dispatch(NULL, 0U, &selected) ==
           D1L_MESHCORE_CHANNEL_DISPATCH_UNKNOWN);
    assert(selected == 0U);

    const bool failed[] = {false, false, false};
    assert(run_dispatch(failed, 3U, &selected) ==
           D1L_MESHCORE_CHANNEL_DISPATCH_AUTH_FAILED);
    assert(selected == 0U);
}

static void test_every_collision_candidate_is_executable(void)
{
    size_t selected = 99U;
    for (size_t accepted = 0U;
         accepted < D1L_MESHCORE_CHANNEL_CANDIDATE_CAPACITY;
         ++accepted) {
        bool authenticated[D1L_MESHCORE_CHANNEL_CANDIDATE_CAPACITY] = {false};
        authenticated[accepted] = true;
        assert(run_dispatch(authenticated,
                            D1L_MESHCORE_CHANNEL_CANDIDATE_CAPACITY,
                            &selected) ==
               D1L_MESHCORE_CHANNEL_DISPATCH_ACCEPTED);
        assert(selected == accepted);
    }
}

static void test_ambiguous_authentication_is_closed(void)
{
    const bool ambiguous[] = {true, false, true};
    size_t selected = 99U;
    assert(run_dispatch(ambiguous, 3U, &selected) ==
           D1L_MESHCORE_CHANNEL_DISPATCH_AMBIGUOUS);
    assert(selected == 0U);
}

static void test_incomplete_or_out_of_order_scan_is_closed(void)
{
    d1l_meshcore_channel_dispatch_t dispatch = {0};
    assert(d1l_meshcore_channel_dispatch_begin(&dispatch, 2U));
    assert(!d1l_meshcore_channel_dispatch_observe(&dispatch, 1U, true));
    assert(d1l_meshcore_channel_dispatch_finish(&dispatch, NULL) ==
           D1L_MESHCORE_CHANNEL_DISPATCH_INVALID);

    assert(d1l_meshcore_channel_dispatch_begin(&dispatch, 2U));
    assert(d1l_meshcore_channel_dispatch_observe(&dispatch, 0U, true));
    assert(d1l_meshcore_channel_dispatch_finish(&dispatch, NULL) ==
           D1L_MESHCORE_CHANNEL_DISPATCH_INVALID);
    assert(!d1l_meshcore_channel_dispatch_begin(
        &dispatch, D1L_MESHCORE_CHANNEL_CANDIDATE_CAPACITY + 1U));
}

static void test_peer_authority_and_self_are_full_key_bound(void)
{
    uint8_t local[32] = {0};
    uint8_t peer[32] = {0};
    local[0] = 0x42U;
    peer[0] = 0x42U;
    peer[31] = 0x01U;

    assert(d1l_meshcore_peer_dispatch_classify(local, peer, false) ==
           D1L_MESHCORE_PEER_UNKNOWN);
    assert(d1l_meshcore_peer_dispatch_classify(local, local, true) ==
           D1L_MESHCORE_PEER_SELF);
    assert(d1l_meshcore_peer_dispatch_classify(local, peer, true) ==
           D1L_MESHCORE_PEER_AUTHORIZED);
    assert(d1l_meshcore_peer_dispatch_classify(NULL, peer, true) ==
           D1L_MESHCORE_PEER_UNKNOWN);
}

int main(void)
{
    assert(d1l_meshcore_peer_text_authorized(1U, false, true));
    assert(!d1l_meshcore_peer_text_authorized(0U, false, true));
    assert(!d1l_meshcore_peer_text_authorized(1U, true, false));
    assert(d1l_meshcore_peer_text_authorized(0U, true, false));
    assert(d1l_meshcore_peer_text_authorized(2U, true, false));
    assert(!d1l_meshcore_peer_text_authorized(3U, true, true));
    test_unknown_and_failed_channel_are_closed();
    test_every_collision_candidate_is_executable();
    test_ambiguous_authentication_is_closed();
    test_incomplete_or_out_of_order_scan_is_closed();
    test_peer_authority_and_self_are_full_key_bound();
    puts("native MeshCore RX admission: ok");
    return 0;
}
