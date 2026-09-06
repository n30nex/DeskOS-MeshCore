#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "meshcore_path_state.h"
#include "meshcore_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    D1L_MESHCORE_ROUTE_SELECTION_NONE = 0,
    D1L_MESHCORE_ROUTE_SELECTION_DIRECT_PROVEN,
    D1L_MESHCORE_ROUTE_SELECTION_FLOOD_NO_PATH,
    D1L_MESHCORE_ROUTE_SELECTION_FLOOD_PREBOOT_PATH,
    D1L_MESHCORE_ROUTE_SELECTION_FLOOD_STALE_PATH,
    D1L_MESHCORE_ROUTE_SELECTION_FLOOD_MALFORMED_PATH,
    D1L_MESHCORE_ROUTE_SELECTION_FLOOD_EXPIRED_PATH,
    D1L_MESHCORE_ROUTE_SELECTION_FLOOD_FAILED_PATH,
    D1L_MESHCORE_ROUTE_SELECTION_FLOOD_DIRECT_RETRY,
} d1l_meshcore_route_selection_reason_t;

typedef struct {
    uint8_t route;
    uint8_t path_len;
    uint8_t path[D1L_MESHCORE_MAX_PATH_BYTES];
    uint8_t path_byte_len;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint32_t path_age_ms;
    uint32_t path_generation;
    d1l_meshcore_route_selection_reason_t reason;
} d1l_meshcore_route_selection_t;

/*
 * Select one immutable outbound route. A canonical learned path follows the
 * pinned MeshCore rule (known, including zero-hop, means direct) only after it
 * has been authenticated during this boot, with DeskOS' required age gate
 * layered on top. Missing, preboot, stale, or malformed learned state always
 * produces a complete flood plan; invalid global policy input leaves the
 * caller's output untouched.
 */
static inline bool d1l_meshcore_route_select(
    bool learned_path_known,
    bool learned_this_boot,
    const uint8_t *learned_path,
    uint8_t learned_path_len,
    uint32_t learned_at_ms,
    uint32_t now_ms,
    uint8_t flood_path_hash_bytes,
    d1l_meshcore_route_selection_t *out_selection)
{
    if (!out_selection || flood_path_hash_bytes < 1U ||
        flood_path_hash_bytes > 3U) {
        return false;
    }

    d1l_meshcore_route_selection_t selection = {0};
    selection.route = D1L_MESHCORE_ROUTE_FLOOD;
    selection.path_len = (uint8_t)((flood_path_hash_bytes - 1U) << 6U);
    selection.path_hash_bytes = flood_path_hash_bytes;
    selection.reason = D1L_MESHCORE_ROUTE_SELECTION_FLOOD_NO_PATH;

    if (learned_path_known) {
        const bool encoded_path_valid =
            d1l_meshcore_wire_path_len_valid(learned_path_len);
        const uint8_t learned_path_bytes = encoded_path_valid ?
            d1l_meshcore_wire_path_byte_len(learned_path_len) : 0U;
        if (!encoded_path_valid ||
            (learned_path_bytes > 0U && learned_path == NULL)) {
            selection.reason = D1L_MESHCORE_ROUTE_SELECTION_FLOOD_MALFORMED_PATH;
        } else if (!learned_this_boot) {
            /* Persisted ESP uptime is not comparable across boots. Never let
             * an old learned_at_ms become fresh again when a later boot reaches
             * the same uptime; only a path authenticated in this boot can be
            * direct. */
            selection.reason = D1L_MESHCORE_ROUTE_SELECTION_FLOOD_PREBOOT_PATH;
        } else {
            selection.path_age_ms = now_ms - learned_at_ms;
            if (selection.path_age_ms >
                D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS) {
                selection.reason =
                    D1L_MESHCORE_ROUTE_SELECTION_FLOOD_STALE_PATH;
            } else {
                selection.route = D1L_MESHCORE_ROUTE_DIRECT;
                selection.path_len = learned_path_len;
                selection.path_byte_len = learned_path_bytes;
                selection.path_hash_bytes =
                    d1l_meshcore_wire_path_hash_size(learned_path_len);
                selection.path_hops =
                    d1l_meshcore_wire_path_hash_count(learned_path_len);
                selection.reason =
                    D1L_MESHCORE_ROUTE_SELECTION_DIRECT_PROVEN;
                if (learned_path_bytes > 0U) {
                    memcpy(selection.path, learned_path, learned_path_bytes);
                }
            }
        }
    }

    *out_selection = selection;
    return true;
}

/* Select from one retained canonical record. Lifecycle state is authoritative:
 * an expired or failure-threshold path produces an exact flood reason even
 * after its privacy-sensitive bytes have been reset by the contact store. */
static inline bool d1l_meshcore_route_select_canonical(
    bool path_known,
    bool learned_this_boot,
    const uint8_t *path,
    uint8_t path_len,
    const d1l_meshcore_path_state_t *path_state,
    uint32_t now_ms,
    uint8_t flood_path_hash_bytes,
    d1l_meshcore_route_selection_t *out_selection)
{
    if (!path_state || !out_selection || flood_path_hash_bytes < 1U ||
        flood_path_hash_bytes > 3U) {
        return false;
    }

    const uint8_t lifecycle = path_state->lifecycle;
    bool select_known = path_known;
    d1l_meshcore_route_selection_reason_t forced_reason =
        D1L_MESHCORE_ROUTE_SELECTION_NONE;
    if (lifecycle == D1L_MESHCORE_PATH_STATE_NONE) {
        select_known = false;
        if (path_known) {
            forced_reason =
                D1L_MESHCORE_ROUTE_SELECTION_FLOOD_MALFORMED_PATH;
        }
    } else if (lifecycle == D1L_MESHCORE_PATH_STATE_EXPIRED) {
        select_known = false;
        forced_reason = D1L_MESHCORE_ROUTE_SELECTION_FLOOD_EXPIRED_PATH;
    } else if (lifecycle == D1L_MESHCORE_PATH_STATE_FAILED) {
        select_known = false;
        forced_reason = D1L_MESHCORE_ROUTE_SELECTION_FLOOD_FAILED_PATH;
    } else if (lifecycle != D1L_MESHCORE_PATH_STATE_VALID || !path_known) {
        select_known = false;
        forced_reason = D1L_MESHCORE_ROUTE_SELECTION_FLOOD_MALFORMED_PATH;
    }

    d1l_meshcore_route_selection_t selection = {0};
    if (!d1l_meshcore_route_select(
            select_known, learned_this_boot, path, path_len,
            d1l_meshcore_path_state_validated_at_ms(path_state), now_ms,
            flood_path_hash_bytes, &selection)) {
        return false;
    }
    if (forced_reason != D1L_MESHCORE_ROUTE_SELECTION_NONE) {
        selection.reason = forced_reason;
        if (forced_reason == D1L_MESHCORE_ROUTE_SELECTION_FLOOD_EXPIRED_PATH ||
            forced_reason == D1L_MESHCORE_ROUTE_SELECTION_FLOOD_FAILED_PATH) {
            selection.path_age_ms = now_ms -
                d1l_meshcore_path_state_validated_at_ms(path_state);
        }
    }
    selection.path_generation = path_state->generation;
    *out_selection = selection;
    return true;
}

/* Server requests may reuse an authenticated PATH response from this boot.
 * An advert's reverse path is not proof of a route to the server. */
static inline bool d1l_meshcore_route_select_admin_request(
    bool path_known, bool learned_this_boot, const uint8_t *path,
    uint8_t path_len, const d1l_meshcore_path_state_t *path_state,
    uint32_t now_ms, uint8_t flood_path_hash_bytes,
    d1l_meshcore_route_selection_t *out_selection)
{
    const bool response_path = path_state &&
        path_state->source == D1L_MESHCORE_PATH_SOURCE_PATH_RESPONSE;
    return d1l_meshcore_route_select_canonical(
        path_known, learned_this_boot && response_path, path, path_len,
        path_state, now_ms, flood_path_hash_bytes, out_selection);
}

static inline const char *d1l_meshcore_route_selection_reason_name(
    d1l_meshcore_route_selection_reason_t reason)
{
    switch (reason) {
    case D1L_MESHCORE_ROUTE_SELECTION_NONE:
        return "none";
    case D1L_MESHCORE_ROUTE_SELECTION_DIRECT_PROVEN:
        return "direct_proven";
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_NO_PATH:
        return "flood_no_path";
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_PREBOOT_PATH:
        return "flood_preboot_path";
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_STALE_PATH:
        return "flood_stale_path";
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_MALFORMED_PATH:
        return "flood_bad_path";
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_EXPIRED_PATH:
        return "flood_expired_path";
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_FAILED_PATH:
        return "flood_failed_path";
    case D1L_MESHCORE_ROUTE_SELECTION_FLOOD_DIRECT_RETRY:
        return "flood_direct_retry";
    default:
        return "unknown";
    }
}

#ifdef __cplusplus
}
#endif
