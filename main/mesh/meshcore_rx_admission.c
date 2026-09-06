#include "mesh/meshcore_rx_admission.h"

#include <string.h>

bool d1l_meshcore_channel_dispatch_begin(
    d1l_meshcore_channel_dispatch_t *dispatch, size_t candidate_count)
{
    if (!dispatch ||
        candidate_count > D1L_MESHCORE_CHANNEL_CANDIDATE_CAPACITY) {
        return false;
    }
    memset(dispatch, 0, sizeof(*dispatch));
    dispatch->candidate_count = candidate_count;
    dispatch->valid = true;
    return true;
}

bool d1l_meshcore_channel_dispatch_observe(
    d1l_meshcore_channel_dispatch_t *dispatch, size_t candidate_index,
    bool authenticated)
{
    if (!dispatch || !dispatch->valid ||
        candidate_index != dispatch->next_candidate ||
        candidate_index >= dispatch->candidate_count) {
        if (dispatch) {
            dispatch->valid = false;
        }
        return false;
    }
    if (authenticated) {
        if (dispatch->authenticated_count == 0U) {
            dispatch->selected_candidate = candidate_index;
        }
        dispatch->authenticated_count++;
    }
    dispatch->next_candidate++;
    return true;
}

d1l_meshcore_channel_dispatch_outcome_t
d1l_meshcore_channel_dispatch_finish(
    const d1l_meshcore_channel_dispatch_t *dispatch,
    size_t *out_selected_candidate)
{
    if (out_selected_candidate) {
        *out_selected_candidate = 0U;
    }
    if (!dispatch || !dispatch->valid ||
        dispatch->next_candidate != dispatch->candidate_count) {
        return D1L_MESHCORE_CHANNEL_DISPATCH_INVALID;
    }
    if (dispatch->candidate_count == 0U) {
        return D1L_MESHCORE_CHANNEL_DISPATCH_UNKNOWN;
    }
    if (dispatch->authenticated_count == 0U) {
        return D1L_MESHCORE_CHANNEL_DISPATCH_AUTH_FAILED;
    }
    if (dispatch->authenticated_count != 1U) {
        return D1L_MESHCORE_CHANNEL_DISPATCH_AMBIGUOUS;
    }
    if (out_selected_candidate) {
        *out_selected_candidate = dispatch->selected_candidate;
    }
    return D1L_MESHCORE_CHANNEL_DISPATCH_ACCEPTED;
}

bool d1l_meshcore_peer_text_authorized(uint8_t text_type,
                                     bool can_message, bool can_admin)
{
    /* MeshCore text types: ordinary=0, CLI=1, signed room post=2. */
    return text_type == 1U ? can_admin :
        ((text_type == 0U || text_type == 2U) && can_message);
}

d1l_meshcore_peer_dispatch_outcome_t d1l_meshcore_peer_dispatch_classify(
    const uint8_t local_public_key[32],
    const uint8_t candidate_public_key[32], bool candidate_authorized)
{
    if (!local_public_key || !candidate_public_key) {
        return D1L_MESHCORE_PEER_UNKNOWN;
    }
    if (memcmp(local_public_key, candidate_public_key, 32U) == 0) {
        return D1L_MESHCORE_PEER_SELF;
    }
    return candidate_authorized ? D1L_MESHCORE_PEER_AUTHORIZED :
                                  D1L_MESHCORE_PEER_UNKNOWN;
}
