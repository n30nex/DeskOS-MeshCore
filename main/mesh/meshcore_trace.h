#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "meshcore_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define D1L_MESHCORE_TRACE_FIXED_BYTES 9U
#define D1L_MESHCORE_TRACE_MAX_HOPS 63U
#define D1L_MESHCORE_TRACE_MAX_HASH_BYTES 8U
#define D1L_MESHCORE_TRACE_MAX_CONTACT_HASH_BYTES 2U
#define D1L_MESHCORE_TRACE_MAX_PATH_BYTES \
    (D1L_MESHCORE_MAX_PACKET_PAYLOAD - D1L_MESHCORE_TRACE_FIXED_BYTES)
#define D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS 30000U
/* Rate-limit the next TRACE from the terminal matched/no-response outcome. */
#define D1L_MESHCORE_TRACE_COOLDOWN_MS 30000U
#define D1L_MESHCORE_TRACE_DUPLICATE_WINDOW_MS 60000U
#define D1L_MESHCORE_TRACE_RETAINED_TARGET "trace_last"

typedef struct {
    uint32_t tag;
    uint32_t auth_code;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint8_t path_hashes[D1L_MESHCORE_TRACE_MAX_PATH_BYTES];
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET];
    uint8_t raw_len;
} d1l_meshcore_trace_source_t;

typedef struct {
    uint32_t tag;
    uint32_t auth_code;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint8_t path_hashes[D1L_MESHCORE_TRACE_MAX_PATH_BYTES];
    int8_t path_snrs_quarter_db[D1L_MESHCORE_TRACE_MAX_HOPS];
} d1l_meshcore_trace_terminal_t;

typedef enum {
    D1L_MESHCORE_TRACE_OUTCOME_NONE = 0,
    D1L_MESHCORE_TRACE_OUTCOME_PENDING,
    D1L_MESHCORE_TRACE_OUTCOME_MATCHED,
    D1L_MESHCORE_TRACE_OUTCOME_NO_RESPONSE,
} d1l_meshcore_trace_outcome_t;

typedef struct {
    bool pending;
    uint32_t pending_tag;
    uint32_t pending_auth_code;
    uint32_t pending_started_ms;
    uint8_t pending_path_hash_bytes;
    uint8_t pending_path_hops;
    uint8_t pending_path_hashes[D1L_MESHCORE_TRACE_MAX_PATH_BYTES];
    bool expired_attempt_valid;
    uint32_t expired_tag;
    uint32_t expired_correlation_code;
    uint32_t expired_at_ms;
    uint8_t expired_path_hash_bytes;
    uint8_t expired_path_hops;
    uint8_t expired_path_hashes[D1L_MESHCORE_TRACE_MAX_PATH_BYTES];
    bool attempt_valid;
    d1l_meshcore_trace_outcome_t attempt_outcome;
    uint32_t attempt_outcome_at_ms;
    bool completed;
    uint32_t completed_at_ms;
    d1l_meshcore_trace_terminal_t last_result;
} d1l_meshcore_trace_tracker_t;

typedef enum {
    D1L_MESHCORE_CONTACT_TRACE_PLAN_OK = 0,
    D1L_MESHCORE_CONTACT_TRACE_PLAN_INVALID,
    D1L_MESHCORE_CONTACT_TRACE_PLAN_UNSUPPORTED_WIDTH,
    D1L_MESHCORE_CONTACT_TRACE_PLAN_EMPTY,
    D1L_MESHCORE_CONTACT_TRACE_PLAN_TOO_LONG,
} d1l_meshcore_contact_trace_plan_result_t;

typedef struct {
    bool includes_contact;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint8_t path_hashes[D1L_MESHCORE_TRACE_MAX_PATH_BYTES];
} d1l_meshcore_contact_trace_plan_t;

typedef enum {
    D1L_MESHCORE_TRACE_CORRELATION_MATCHED = 0,
    D1L_MESHCORE_TRACE_CORRELATION_DUPLICATE,
    D1L_MESHCORE_TRACE_CORRELATION_UNMATCHED,
    D1L_MESHCORE_TRACE_CORRELATION_EXPIRED,
    D1L_MESHCORE_TRACE_CORRELATION_AUTH_MISMATCH,
    D1L_MESHCORE_TRACE_CORRELATION_PATH_MISMATCH,
} d1l_meshcore_trace_correlation_t;

typedef enum {
    D1L_MESHCORE_TRACE_FRAME_MALFORMED = 0,
    D1L_MESHCORE_TRACE_FRAME_UNSUPPORTED,
    D1L_MESHCORE_TRACE_FRAME_SOURCE,
    D1L_MESHCORE_TRACE_FRAME_IN_FLIGHT,
    D1L_MESHCORE_TRACE_FRAME_TERMINAL,
} d1l_meshcore_trace_frame_kind_t;

static inline void d1l_meshcore_trace_write_le32(uint8_t *dest,
                                                 uint32_t value)
{
    dest[0] = (uint8_t)(value & 0xffU);
    dest[1] = (uint8_t)((value >> 8U) & 0xffU);
    dest[2] = (uint8_t)((value >> 16U) & 0xffU);
    dest[3] = (uint8_t)(value >> 24U);
}

static inline uint32_t d1l_meshcore_trace_read_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8U) |
           ((uint32_t)src[2] << 16U) |
           ((uint32_t)src[3] << 24U);
}

static inline bool d1l_meshcore_trace_hash_width_supported(
    uint8_t path_hash_bytes)
{
    return path_hash_bytes > 0U &&
           path_hash_bytes <= D1L_MESHCORE_TRACE_MAX_HASH_BYTES &&
           (path_hash_bytes & (path_hash_bytes - 1U)) == 0U;
}

static inline uint8_t d1l_meshcore_trace_flags_for_hash_width(
    uint8_t path_hash_bytes)
{
    return path_hash_bytes == 1U ? 0U :
           path_hash_bytes == 2U ? 1U :
           path_hash_bytes == 4U ? 2U : 3U;
}

/*
 * MeshCore route packets encode one-, two-, or three-byte public-key
 * prefixes, while TRACE v1.11 encodes only power-of-two prefix widths.
 * A three-byte route can therefore be represented without changing its hops
 * by using each hash's first two bytes. Identity::isHashMatch compares exactly
 * that public-key prefix.
 */
static inline uint8_t d1l_meshcore_trace_contact_hash_width(
    uint8_t route_hash_bytes)
{
    return route_hash_bytes == 1U ? 1U :
           route_hash_bytes == 2U || route_hash_bytes == 3U ? 2U : 0U;
}

/*
 * Derive the only contact TRACE loop accepted by DeskOS. The caller must
 * first authorize one immutable, current-boot direct route. One- and two-byte
 * route hashes are copied in transmit order; three-byte route hashes are
 * normalized to their exact two-byte public-key prefixes. Repeater and room
 * contacts can forward TRACE, so their exact public-key hash prefix is the
 * pivot; chat and sensor contacts cannot, so the farthest proven repeater is
 * the pivot. The return leg omits that pivot and reverses only whole hashes
 * from the already authorized route. No caller-supplied arbitrary loop can
 * enter this helper.
 */
static inline d1l_meshcore_contact_trace_plan_result_t
d1l_meshcore_trace_plan_contact(
    const uint8_t *out_path,
    uint8_t out_path_len,
    bool contact_forwards_trace,
    const uint8_t *contact_hash,
    d1l_meshcore_contact_trace_plan_t *out_plan)
{
    if (!out_plan || !d1l_meshcore_wire_path_len_valid(out_path_len)) {
        return D1L_MESHCORE_CONTACT_TRACE_PLAN_INVALID;
    }
    const uint8_t route_hash_bytes =
        d1l_meshcore_wire_path_hash_size(out_path_len);
    const uint8_t trace_hash_bytes =
        d1l_meshcore_trace_contact_hash_width(route_hash_bytes);
    const uint8_t path_hops =
        d1l_meshcore_wire_path_hash_count(out_path_len);
    const uint8_t path_bytes =
        d1l_meshcore_wire_path_byte_len(out_path_len);
    if (trace_hash_bytes == 0U ||
        trace_hash_bytes > D1L_MESHCORE_TRACE_MAX_CONTACT_HASH_BYTES) {
        return D1L_MESHCORE_CONTACT_TRACE_PLAN_UNSUPPORTED_WIDTH;
    }
    if (path_bytes > 0U && !out_path) {
        return D1L_MESHCORE_CONTACT_TRACE_PLAN_INVALID;
    }
    if (contact_forwards_trace && !contact_hash) {
        return D1L_MESHCORE_CONTACT_TRACE_PLAN_INVALID;
    }
    if (path_hops == 0U && !contact_forwards_trace) {
        return D1L_MESHCORE_CONTACT_TRACE_PLAN_EMPTY;
    }

    const size_t loop_hops = contact_forwards_trace ?
        (size_t)path_hops * 2U + 1U :
        (size_t)path_hops * 2U - 1U;
    if (loop_hops > D1L_MESHCORE_TRACE_MAX_HOPS) {
        return D1L_MESHCORE_CONTACT_TRACE_PLAN_TOO_LONG;
    }

    d1l_meshcore_contact_trace_plan_t plan = {
        .includes_contact = contact_forwards_trace,
        .path_hash_bytes = trace_hash_bytes,
        .path_hops = (uint8_t)loop_hops,
    };
    const size_t loop_bytes = loop_hops * trace_hash_bytes;
    if (loop_bytes > sizeof(plan.path_hashes)) {
        return D1L_MESHCORE_CONTACT_TRACE_PLAN_TOO_LONG;
    }
    for (size_t i = 0U; i < path_hops; ++i) {
        memcpy(&plan.path_hashes[i * trace_hash_bytes],
               &out_path[i * route_hash_bytes], trace_hash_bytes);
    }
    size_t write_hop = path_hops;
    if (contact_forwards_trace) {
        memcpy(&plan.path_hashes[write_hop * trace_hash_bytes],
               contact_hash, trace_hash_bytes);
        write_hop++;
        for (size_t i = path_hops; i > 0U; --i) {
            memcpy(&plan.path_hashes[write_hop * trace_hash_bytes],
                   &out_path[(i - 1U) * route_hash_bytes],
                   trace_hash_bytes);
            write_hop++;
        }
    } else {
        for (size_t i = path_hops - 1U; i > 0U; --i) {
            memcpy(&plan.path_hashes[write_hop * trace_hash_bytes],
                   &out_path[(i - 1U) * route_hash_bytes],
                   trace_hash_bytes);
            write_hop++;
        }
    }
    if (write_hop != loop_hops) {
        return D1L_MESHCORE_CONTACT_TRACE_PLAN_INVALID;
    }
    *out_plan = plan;
    return D1L_MESHCORE_CONTACT_TRACE_PLAN_OK;
}

/* Every completed tag replaces the same bounded route-store summary row. */
static inline const char *d1l_meshcore_trace_retained_target_for_tag(
    uint32_t tag)
{
    (void)tag;
    return D1L_MESHCORE_TRACE_RETAINED_TARGET;
}

/*
 * Build the exact public source frame from pinned Mesh::createTrace followed
 * by Mesh::sendDirect. TRACE is never flood-routed: its explicit
 * hashes are appended to the payload and the outer path starts empty so
 * repeaters can append one SNR sample per hop there. Pinned MeshCore v1.11+
 * stores log2(hash-width) in the TRACE flags low bits. The generic helper
 * accepts the complete pinned 1/2/4/8-byte wire set, while the contact planner
 * can derive only the 1/2-byte subset from canonical SIGUI contact routes.
 */
static inline bool d1l_meshcore_trace_build_source(
    uint32_t tag,
    uint32_t auth_code,
    uint8_t path_hash_bytes,
    const uint8_t *path_hashes,
    size_t path_hops,
    d1l_meshcore_trace_source_t *out_source)
{
    const size_t path_bytes = path_hops * path_hash_bytes;
    if (!out_source || path_hops > D1L_MESHCORE_TRACE_MAX_HOPS ||
        !d1l_meshcore_trace_hash_width_supported(path_hash_bytes) ||
        path_bytes > D1L_MESHCORE_TRACE_MAX_PATH_BYTES ||
        (path_bytes > 0U && !path_hashes)) {
        return false;
    }

    d1l_meshcore_trace_source_t source = {0};
    size_t raw_len = 0U;
    const uint8_t header = (uint8_t)(
        (D1L_MESHCORE_PAYLOAD_TRACE << 2U) | D1L_MESHCORE_ROUTE_DIRECT);
    if (!d1l_meshcore_wire_write_prefix(
            header, 0U, 0U, 0U, NULL, source.raw, sizeof(source.raw),
            &raw_len) ||
        raw_len + D1L_MESHCORE_TRACE_FIXED_BYTES + path_bytes >
            D1L_MESHCORE_MAX_RAW_PACKET) {
        return false;
    }

    source.tag = tag;
    source.auth_code = auth_code;
    source.path_hash_bytes = path_hash_bytes;
    source.path_hops = (uint8_t)path_hops;
    if (path_bytes > 0U) {
        memcpy(source.path_hashes, path_hashes, path_bytes);
    }
    d1l_meshcore_trace_write_le32(&source.raw[raw_len], tag);
    raw_len += 4U;
    d1l_meshcore_trace_write_le32(&source.raw[raw_len], auth_code);
    raw_len += 4U;
    source.raw[raw_len++] =
        d1l_meshcore_trace_flags_for_hash_width(path_hash_bytes);
    if (path_bytes > 0U) {
        memcpy(&source.raw[raw_len], path_hashes, path_bytes);
        raw_len += path_bytes;
    }
    source.raw_len = (uint8_t)raw_len;
    *out_source = source;
    return true;
}

/* Classify direct TRACE without treating valid source/in-flight copies as
 * malformed. Reserved high flag bits remain explicitly unsupported. */
static inline d1l_meshcore_trace_frame_kind_t d1l_meshcore_trace_classify(
    const uint8_t *raw,
    size_t raw_len,
    d1l_meshcore_trace_terminal_t *out_terminal)
{
    if (!raw) {
        return D1L_MESHCORE_TRACE_FRAME_MALFORMED;
    }

    d1l_meshcore_wire_packet_t packet = {0};
    if (!d1l_meshcore_wire_decode_v1(raw, raw_len, &packet) ||
        packet.route != D1L_MESHCORE_ROUTE_DIRECT ||
        packet.type != D1L_MESHCORE_PAYLOAD_TRACE ||
        packet.path_hash_bytes != 1U ||
        packet.payload_len < D1L_MESHCORE_TRACE_FIXED_BYTES) {
        return D1L_MESHCORE_TRACE_FRAME_MALFORMED;
    }

    const uint8_t flags = packet.payload[8];
    const size_t explicit_path_bytes =
        packet.payload_len - D1L_MESHCORE_TRACE_FIXED_BYTES;
    const size_t explicit_hash_bytes = 1U << (flags & 0x03U);
    if (explicit_path_bytes == 0U) {
        return D1L_MESHCORE_TRACE_FRAME_UNSUPPORTED;
    }
    if ((explicit_path_bytes % explicit_hash_bytes) != 0U) {
        return D1L_MESHCORE_TRACE_FRAME_MALFORMED;
    }
    const size_t explicit_hops = explicit_path_bytes / explicit_hash_bytes;
    if (explicit_hops == 0U ||
        explicit_hops > D1L_MESHCORE_TRACE_MAX_HOPS ||
        packet.path_byte_len != packet.path_hops ||
        packet.path_hops > explicit_hops) {
        return D1L_MESHCORE_TRACE_FRAME_MALFORMED;
    }
    if ((flags & 0xfcU) != 0U ||
        explicit_path_bytes > D1L_MESHCORE_TRACE_MAX_PATH_BYTES) {
        return D1L_MESHCORE_TRACE_FRAME_UNSUPPORTED;
    }
    if (packet.path_hops == 0U) {
        return D1L_MESHCORE_TRACE_FRAME_SOURCE;
    }
    if (packet.path_hops < explicit_hops) {
        return D1L_MESHCORE_TRACE_FRAME_IN_FLIGHT;
    }

    d1l_meshcore_trace_terminal_t terminal = {0};
    terminal.tag = d1l_meshcore_trace_read_le32(packet.payload);
    terminal.auth_code = d1l_meshcore_trace_read_le32(&packet.payload[4]);
    terminal.path_hash_bytes = (uint8_t)explicit_hash_bytes;
    terminal.path_hops = (uint8_t)explicit_hops;
    memcpy(terminal.path_hashes,
           &packet.payload[D1L_MESHCORE_TRACE_FIXED_BYTES],
           explicit_path_bytes);
    memcpy(terminal.path_snrs_quarter_db, packet.path, explicit_hops);
    if (out_terminal) {
        *out_terminal = terminal;
    }
    return D1L_MESHCORE_TRACE_FRAME_TERMINAL;
}

/* Parse only a canonical terminal TRACE frame with a supported hash width. */
static inline bool d1l_meshcore_trace_parse_terminal(
    const uint8_t *raw,
    size_t raw_len,
    d1l_meshcore_trace_terminal_t *out_terminal)
{
    return out_terminal &&
           d1l_meshcore_trace_classify(raw, raw_len, out_terminal) ==
               D1L_MESHCORE_TRACE_FRAME_TERMINAL;
}

static inline const char *d1l_meshcore_trace_outcome_name(
    d1l_meshcore_trace_outcome_t outcome)
{
    switch (outcome) {
    case D1L_MESHCORE_TRACE_OUTCOME_PENDING:
        return "pending";
    case D1L_MESHCORE_TRACE_OUTCOME_MATCHED:
        return "matched";
    case D1L_MESHCORE_TRACE_OUTCOME_NO_RESPONSE:
        return "no_response";
    case D1L_MESHCORE_TRACE_OUTCOME_NONE:
    default:
        return "none";
    }
}

static inline uint32_t d1l_meshcore_trace_tracker_cooldown_remaining_ms(
    const d1l_meshcore_trace_tracker_t *tracker,
    uint32_t now_ms)
{
    if (!tracker || !tracker->attempt_valid ||
        (tracker->attempt_outcome != D1L_MESHCORE_TRACE_OUTCOME_MATCHED &&
         tracker->attempt_outcome !=
             D1L_MESHCORE_TRACE_OUTCOME_NO_RESPONSE)) {
        return 0U;
    }
    const uint32_t elapsed =
        (uint32_t)(now_ms - tracker->attempt_outcome_at_ms);
    return elapsed >= D1L_MESHCORE_TRACE_COOLDOWN_MS ?
        0U : D1L_MESHCORE_TRACE_COOLDOWN_MS - elapsed;
}

static inline bool d1l_meshcore_trace_tracker_expire_pending(
    d1l_meshcore_trace_tracker_t *tracker,
    uint32_t now_ms)
{
    if (!tracker || !tracker->pending ||
        (uint32_t)(now_ms - tracker->pending_started_ms) <
            D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS) {
        return false;
    }
    tracker->pending = false;
    tracker->expired_attempt_valid = true;
    tracker->expired_tag = tracker->pending_tag;
    tracker->expired_correlation_code = tracker->pending_auth_code;
    tracker->expired_at_ms = now_ms;
    tracker->expired_path_hash_bytes = tracker->pending_path_hash_bytes;
    tracker->expired_path_hops = tracker->pending_path_hops;
    memcpy(tracker->expired_path_hashes,
           tracker->pending_path_hashes,
           sizeof(tracker->expired_path_hashes));
    tracker->attempt_valid = true;
    tracker->attempt_outcome = D1L_MESHCORE_TRACE_OUTCOME_NO_RESPONSE;
    tracker->attempt_outcome_at_ms = now_ms;
    return true;
}

static inline bool d1l_meshcore_trace_tracker_begin(
    d1l_meshcore_trace_tracker_t *tracker,
    uint32_t tag,
    uint32_t auth_code,
    uint8_t path_hash_bytes,
    const uint8_t *path_hashes,
    size_t path_hops,
    uint32_t now_ms)
{
    const size_t path_bytes = path_hops * path_hash_bytes;
    if (!tracker || tracker->pending ||
        d1l_meshcore_trace_tracker_cooldown_remaining_ms(tracker, now_ms) > 0U ||
        (tracker->attempt_valid &&
         tracker->attempt_outcome != D1L_MESHCORE_TRACE_OUTCOME_MATCHED &&
         tracker->attempt_outcome !=
             D1L_MESHCORE_TRACE_OUTCOME_NO_RESPONSE) ||
        path_hops > D1L_MESHCORE_TRACE_MAX_HOPS ||
        !d1l_meshcore_trace_hash_width_supported(path_hash_bytes) ||
        path_bytes > D1L_MESHCORE_TRACE_MAX_PATH_BYTES ||
        (path_bytes > 0U && !path_hashes)) {
        return false;
    }
    tracker->pending = true;
    tracker->pending_tag = tag;
    tracker->pending_auth_code = auth_code;
    tracker->pending_started_ms = now_ms;
    tracker->pending_path_hash_bytes = path_hash_bytes;
    tracker->pending_path_hops = (uint8_t)path_hops;
    tracker->attempt_valid = true;
    tracker->attempt_outcome = D1L_MESHCORE_TRACE_OUTCOME_PENDING;
    tracker->attempt_outcome_at_ms = now_ms;
    memset(tracker->pending_path_hashes, 0,
           sizeof(tracker->pending_path_hashes));
    if (path_bytes > 0U) {
        memcpy(tracker->pending_path_hashes, path_hashes, path_bytes);
    }
    return true;
}

static inline bool d1l_meshcore_trace_tracker_cancel(
    d1l_meshcore_trace_tracker_t *tracker,
    uint32_t tag,
    uint32_t auth_code)
{
    if (!tracker || !tracker->pending || tracker->pending_tag != tag ||
        tracker->pending_auth_code != auth_code) {
        return false;
    }
    tracker->pending = false;
    tracker->attempt_valid = false;
    tracker->attempt_outcome = D1L_MESHCORE_TRACE_OUTCOME_NONE;
    tracker->attempt_outcome_at_ms = 0U;
    tracker->pending_tag = 0U;
    tracker->pending_auth_code = 0U;
    tracker->pending_started_ms = 0U;
    tracker->pending_path_hash_bytes = 0U;
    tracker->pending_path_hops = 0U;
    memset(tracker->pending_path_hashes, 0,
           sizeof(tracker->pending_path_hashes));
    return true;
}

static inline bool d1l_meshcore_trace_path_matches(
    const d1l_meshcore_trace_terminal_t *terminal,
    uint8_t expected_hash_bytes,
    uint8_t expected_hops,
    const uint8_t *expected_hashes)
{
    const size_t expected_bytes =
        (size_t)expected_hash_bytes * expected_hops;
    return terminal &&
           terminal->path_hash_bytes == expected_hash_bytes &&
           terminal->path_hops == expected_hops &&
           (expected_hops == 0U ||
            (expected_hashes &&
             memcmp(terminal->path_hashes, expected_hashes,
                    expected_bytes) == 0));
}

static inline d1l_meshcore_trace_correlation_t
d1l_meshcore_trace_tracker_consume(
    d1l_meshcore_trace_tracker_t *tracker,
    const d1l_meshcore_trace_terminal_t *terminal,
    uint32_t now_ms)
{
    if (!tracker || !terminal) {
        return D1L_MESHCORE_TRACE_CORRELATION_UNMATCHED;
    }

    if (tracker->expired_attempt_valid &&
        (uint32_t)(now_ms - tracker->expired_at_ms) <
            D1L_MESHCORE_TRACE_DUPLICATE_WINDOW_MS &&
        terminal->tag == tracker->expired_tag &&
        terminal->auth_code == tracker->expired_correlation_code &&
        d1l_meshcore_trace_path_matches(
            terminal, tracker->expired_path_hash_bytes,
            tracker->expired_path_hops,
            tracker->expired_path_hashes)) {
        return D1L_MESHCORE_TRACE_CORRELATION_EXPIRED;
    }

    if (tracker->pending) {
        const bool expired =
            (uint32_t)(now_ms - tracker->pending_started_ms) >=
            D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS;
        const bool tag_matches = terminal->tag == tracker->pending_tag;
        const bool auth_matches =
            terminal->auth_code == tracker->pending_auth_code;
        const bool path_matches = d1l_meshcore_trace_path_matches(
            terminal, tracker->pending_path_hash_bytes,
            tracker->pending_path_hops,
            tracker->pending_path_hashes);
        if (expired) {
            (void)d1l_meshcore_trace_tracker_expire_pending(tracker, now_ms);
            if (tag_matches && auth_matches && path_matches) {
                return D1L_MESHCORE_TRACE_CORRELATION_EXPIRED;
            }
        } else if (tag_matches) {
            if (!auth_matches) {
                return D1L_MESHCORE_TRACE_CORRELATION_AUTH_MISMATCH;
            }
            if (!path_matches) {
                return D1L_MESHCORE_TRACE_CORRELATION_PATH_MISMATCH;
            }
            tracker->pending = false;
            tracker->attempt_valid = true;
            tracker->attempt_outcome = D1L_MESHCORE_TRACE_OUTCOME_MATCHED;
            tracker->attempt_outcome_at_ms = now_ms;
            tracker->completed = true;
            tracker->completed_at_ms = now_ms;
            tracker->last_result = *terminal;
            return D1L_MESHCORE_TRACE_CORRELATION_MATCHED;
        }
    }

    if (tracker->completed &&
        (uint32_t)(now_ms - tracker->completed_at_ms) <
            D1L_MESHCORE_TRACE_DUPLICATE_WINDOW_MS &&
        terminal->tag == tracker->last_result.tag &&
        terminal->auth_code == tracker->last_result.auth_code &&
        d1l_meshcore_trace_path_matches(
            terminal, tracker->last_result.path_hash_bytes,
            tracker->last_result.path_hops,
            tracker->last_result.path_hashes)) {
        return D1L_MESHCORE_TRACE_CORRELATION_DUPLICATE;
    }

    return D1L_MESHCORE_TRACE_CORRELATION_UNMATCHED;
}

#ifdef __cplusplus
}
#endif
