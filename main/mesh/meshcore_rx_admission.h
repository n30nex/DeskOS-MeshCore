#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D1L_MESHCORE_CHANNEL_CANDIDATE_CAPACITY 8U

bool d1l_meshcore_peer_text_authorized(uint8_t text_type,
                                     bool can_message, bool can_admin);

typedef enum {
    D1L_MESHCORE_CHANNEL_DISPATCH_INVALID = 0,
    D1L_MESHCORE_CHANNEL_DISPATCH_UNKNOWN,
    D1L_MESHCORE_CHANNEL_DISPATCH_AUTH_FAILED,
    D1L_MESHCORE_CHANNEL_DISPATCH_ACCEPTED,
    D1L_MESHCORE_CHANNEL_DISPATCH_AMBIGUOUS,
} d1l_meshcore_channel_dispatch_outcome_t;

typedef struct {
    size_t candidate_count;
    size_t next_candidate;
    size_t authenticated_count;
    size_t selected_candidate;
    bool valid;
} d1l_meshcore_channel_dispatch_t;

/* A channel hash is routing metadata, not identity. Every configured hash
 * match must be authenticated before exactly one candidate can be selected. */
bool d1l_meshcore_channel_dispatch_begin(
    d1l_meshcore_channel_dispatch_t *dispatch, size_t candidate_count);

bool d1l_meshcore_channel_dispatch_observe(
    d1l_meshcore_channel_dispatch_t *dispatch, size_t candidate_index,
    bool authenticated);

d1l_meshcore_channel_dispatch_outcome_t
d1l_meshcore_channel_dispatch_finish(
    const d1l_meshcore_channel_dispatch_t *dispatch,
    size_t *out_selected_candidate);

typedef enum {
    D1L_MESHCORE_PEER_UNKNOWN = 0,
    D1L_MESHCORE_PEER_SELF,
    D1L_MESHCORE_PEER_AUTHORIZED,
} d1l_meshcore_peer_dispatch_outcome_t;

/* Full identity is authoritative. A retained row that is not currently DM
 * authorized remains unknown, and the local identity is never a DM peer. */
d1l_meshcore_peer_dispatch_outcome_t d1l_meshcore_peer_dispatch_classify(
    const uint8_t local_public_key[32],
    const uint8_t candidate_public_key[32], bool candidate_authorized);

#ifdef __cplusplus
}
#endif
