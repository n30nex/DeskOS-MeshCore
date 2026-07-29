#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mesh/meshcore_trace.h"

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,          \
                    __LINE__, #condition);                                      \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static size_t build_trace_frame(uint32_t tag, uint32_t auth_code,
                                uint8_t flags,
                                const uint8_t *explicit_path,
                                size_t explicit_path_bytes,
                                const int8_t *snrs,
                                size_t outer_hops,
                                uint8_t *raw)
{
    size_t index = 0U;
    raw[index++] = (uint8_t)((D1L_MESHCORE_PAYLOAD_TRACE << 2U) |
                             D1L_MESHCORE_ROUTE_DIRECT);
    raw[index++] = (uint8_t)outer_hops;
    memcpy(&raw[index], snrs, outer_hops);
    index += outer_hops;
    d1l_meshcore_trace_write_le32(&raw[index], tag);
    index += 4U;
    d1l_meshcore_trace_write_le32(&raw[index], auth_code);
    index += 4U;
    raw[index++] = flags;
    memcpy(&raw[index], explicit_path, explicit_path_bytes);
    return index + explicit_path_bytes;
}

static size_t build_terminal(uint32_t tag, uint32_t auth_code,
                             const uint8_t *hashes, const int8_t *snrs,
                             size_t hops, uint8_t *raw)
{
    return build_trace_frame(tag, auth_code, 0U, hashes, hops, snrs, hops,
                             raw);
}

static int test_contact_plan(void)
{
    const uint8_t direct_path[] = {0x11U, 0x22U, 0x33U};
    const uint8_t original_path[] = {0x11U, 0x22U, 0x33U};
    const uint8_t contact_hash[] = {0x44U, 0x45U};
    d1l_meshcore_contact_trace_plan_t plan = {0};
    CHECK(d1l_meshcore_trace_plan_contact(
              direct_path, 3U, false, NULL, &plan) ==
          D1L_MESHCORE_CONTACT_TRACE_PLAN_OK);
    const uint8_t chat_expected[] = {
        0x11U, 0x22U, 0x33U, 0x22U, 0x11U,
    };
    CHECK(!plan.includes_contact);
    CHECK(plan.path_hash_bytes == 1U);
    CHECK(plan.path_hops == sizeof(chat_expected));
    CHECK(memcmp(plan.path_hashes, chat_expected,
                 sizeof(chat_expected)) == 0);

    memset(&plan, 0, sizeof(plan));
    CHECK(d1l_meshcore_trace_plan_contact(
              direct_path, 3U, true, contact_hash, &plan) ==
          D1L_MESHCORE_CONTACT_TRACE_PLAN_OK);
    const uint8_t server_expected[] = {
        0x11U, 0x22U, 0x33U, 0x44U,
        0x33U, 0x22U, 0x11U,
    };
    CHECK(plan.includes_contact);
    CHECK(plan.path_hash_bytes == 1U);
    CHECK(plan.path_hops == sizeof(server_expected));
    CHECK(memcmp(plan.path_hashes, server_expected,
                 sizeof(server_expected)) == 0);
    CHECK(memcmp(direct_path, original_path, sizeof(direct_path)) == 0);

    memset(&plan, 0, sizeof(plan));
    CHECK(d1l_meshcore_trace_plan_contact(
              NULL, 0U, true, contact_hash, &plan) ==
          D1L_MESHCORE_CONTACT_TRACE_PLAN_OK);
    CHECK(plan.includes_contact && plan.path_hops == 1U &&
          plan.path_hash_bytes == 1U &&
          plan.path_hashes[0] == contact_hash[0]);

    const uint8_t two_byte_path[] = {
        0x11U, 0x12U, 0x21U, 0x22U, 0x31U, 0x32U,
    };
    const uint8_t two_byte_expected[] = {
        0x11U, 0x12U, 0x21U, 0x22U, 0x31U, 0x32U, 0x44U,
        0x45U, 0x31U, 0x32U, 0x21U, 0x22U, 0x11U, 0x12U,
    };
    memset(&plan, 0, sizeof(plan));
    CHECK(d1l_meshcore_trace_plan_contact(
              two_byte_path, (uint8_t)(0x40U | 3U), true, contact_hash,
              &plan) == D1L_MESHCORE_CONTACT_TRACE_PLAN_OK);
    CHECK(plan.includes_contact);
    CHECK(plan.path_hash_bytes == 2U);
    CHECK(plan.path_hops == 7U);
    CHECK(memcmp(plan.path_hashes, two_byte_expected,
                 sizeof(two_byte_expected)) == 0);

    const uint8_t three_byte_path[] = {
        0x11U, 0x12U, 0x13U, 0x21U, 0x22U, 0x23U,
    };
    const uint8_t three_byte_expected[] = {
        0x11U, 0x12U, 0x21U, 0x22U, 0x44U,
        0x45U, 0x21U, 0x22U, 0x11U, 0x12U,
    };
    memset(&plan, 0, sizeof(plan));
    CHECK(d1l_meshcore_trace_plan_contact(
              three_byte_path, (uint8_t)(0x80U | 2U), true, contact_hash,
              &plan) == D1L_MESHCORE_CONTACT_TRACE_PLAN_OK);
    CHECK(plan.includes_contact);
    CHECK(plan.path_hash_bytes == 2U);
    CHECK(plan.path_hops == 5U);
    CHECK(memcmp(plan.path_hashes, three_byte_expected,
                 sizeof(three_byte_expected)) == 0);
    CHECK(d1l_meshcore_trace_contact_hash_width(1U) == 1U);
    CHECK(d1l_meshcore_trace_contact_hash_width(2U) == 2U);
    CHECK(d1l_meshcore_trace_contact_hash_width(3U) == 2U);
    CHECK(d1l_meshcore_trace_contact_hash_width(0U) == 0U);
    CHECK(d1l_meshcore_trace_contact_hash_width(4U) == 0U);

    d1l_meshcore_contact_trace_plan_t unchanged;
    memset(&unchanged, 0xA5, sizeof(unchanged));
    const d1l_meshcore_contact_trace_plan_t before = unchanged;
    CHECK(d1l_meshcore_trace_plan_contact(
              NULL, 0U, false, NULL, &unchanged) ==
          D1L_MESHCORE_CONTACT_TRACE_PLAN_EMPTY);
    CHECK(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);
    CHECK(d1l_meshcore_trace_plan_contact(
              direct_path, (uint8_t)(0xC0U | 3U), true, contact_hash,
              &unchanged) ==
          D1L_MESHCORE_CONTACT_TRACE_PLAN_INVALID);
    CHECK(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);
    CHECK(d1l_meshcore_trace_plan_contact(
              direct_path, 3U, true, contact_hash, NULL) ==
          D1L_MESHCORE_CONTACT_TRACE_PLAN_INVALID);
    CHECK(d1l_meshcore_trace_plan_contact(
              NULL, 1U, false, NULL, &unchanged) ==
          D1L_MESHCORE_CONTACT_TRACE_PLAN_INVALID);
    CHECK(d1l_meshcore_trace_plan_contact(
              direct_path, 3U, true, NULL, &unchanged) ==
          D1L_MESHCORE_CONTACT_TRACE_PLAN_INVALID);
    CHECK(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);

    uint8_t long_path[32];
    for (size_t i = 0U; i < sizeof(long_path); ++i) {
        long_path[i] = (uint8_t)(i + 1U);
    }
    CHECK(d1l_meshcore_trace_plan_contact(
              long_path, 32U, false, NULL, &plan) ==
          D1L_MESHCORE_CONTACT_TRACE_PLAN_OK);
    CHECK(plan.path_hops == D1L_MESHCORE_TRACE_MAX_HOPS);
    CHECK(d1l_meshcore_trace_plan_contact(
              long_path, 32U, true, contact_hash, &unchanged) ==
          D1L_MESHCORE_CONTACT_TRACE_PLAN_TOO_LONG);
    CHECK(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);
    return 0;
}

static int test_source_and_terminal(void)
{
    CHECK(strcmp(D1L_MESHCORE_TRACE_RETAINED_TARGET, "trace_last") == 0);
    const char *first_target =
        d1l_meshcore_trace_retained_target_for_tag(0x11111111U);
    const char *second_target =
        d1l_meshcore_trace_retained_target_for_tag(0x22222222U);
    CHECK(strcmp(first_target, "trace_last") == 0);
    CHECK(strcmp(first_target, second_target) == 0);

    uint8_t hashes[] = {0x11U, 0x22U, 0x33U};
    const uint8_t original_hashes[] = {0x11U, 0x22U, 0x33U};
    d1l_meshcore_trace_source_t source = {0};
    CHECK(d1l_meshcore_trace_build_source(
        0x12345678U, 0xA1B2C3D4U, 1U, hashes, 3U, &source));
    const uint8_t expected[] = {
        0x26U, 0x00U, 0x78U, 0x56U, 0x34U, 0x12U,
        0xD4U, 0xC3U, 0xB2U, 0xA1U, 0x00U, 0x11U,
        0x22U, 0x33U,
    };
    CHECK(source.raw_len == sizeof(expected));
    CHECK(source.path_hash_bytes == 1U);
    CHECK(memcmp(source.raw, expected, sizeof(expected)) == 0);
    hashes[0] = 0xEEU;
    CHECK(memcmp(source.path_hashes, original_hashes,
                 sizeof(original_hashes)) == 0);
    CHECK(source.raw[11] == 0x11U);

    d1l_meshcore_trace_terminal_t unchanged;
    memset(&unchanged, 0xA5, sizeof(unchanged));
    const d1l_meshcore_trace_terminal_t before = unchanged;
    CHECK(d1l_meshcore_trace_classify(source.raw, source.raw_len,
                                      &unchanged) ==
          D1L_MESHCORE_TRACE_FRAME_SOURCE);
    CHECK(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);

    CHECK(!d1l_meshcore_trace_build_source(
        1U, 2U, 1U, NULL, 1U, &source));
    CHECK(!d1l_meshcore_trace_build_source(
        1U, 2U, 1U, original_hashes,
        D1L_MESHCORE_TRACE_MAX_HOPS + 1U,
        &source));
    CHECK(!d1l_meshcore_trace_build_source(
        1U, 2U, 3U, original_hashes, 1U, &source));
    CHECK(!d1l_meshcore_trace_build_source(
        1U, 2U, 1U, original_hashes, 1U, NULL));

    const uint8_t two_byte_source_hashes[] = {
        0x11U, 0x12U, 0x22U, 0x23U,
    };
    CHECK(d1l_meshcore_trace_build_source(
        0x01020304U, 0x05060708U, 2U, two_byte_source_hashes, 2U,
        &source));
    const uint8_t two_byte_source_expected[] = {
        0x26U, 0x00U, 0x04U, 0x03U, 0x02U, 0x01U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x01U, 0x11U,
        0x12U, 0x22U, 0x23U,
    };
    CHECK(source.path_hash_bytes == 2U);
    CHECK(source.path_hops == 2U);
    CHECK(source.raw_len == sizeof(two_byte_source_expected));
    CHECK(memcmp(source.raw, two_byte_source_expected,
                 sizeof(two_byte_source_expected)) == 0);

    const uint8_t wide_source_hashes[] = {
        0x10U, 0x11U, 0x12U, 0x13U, 0x20U, 0x21U, 0x22U, 0x23U,
        0x30U, 0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U,
    };
    CHECK(d1l_meshcore_trace_build_source(
        1U, 2U, 4U, wide_source_hashes, 2U, &source));
    CHECK(source.path_hash_bytes == 4U);
    CHECK(source.path_hops == 2U);
    CHECK(source.raw[10] == 2U);
    CHECK(memcmp(&source.raw[11], wide_source_hashes, 8U) == 0);
    CHECK(d1l_meshcore_trace_build_source(
        1U, 2U, 8U, wide_source_hashes, 2U, &source));
    CHECK(source.path_hash_bytes == 8U);
    CHECK(source.path_hops == 2U);
    CHECK(source.raw[10] == 3U);
    CHECK(memcmp(&source.raw[11], wide_source_hashes,
                 sizeof(wide_source_hashes)) == 0);
    uint8_t oversized_path[D1L_MESHCORE_TRACE_MAX_PATH_BYTES + 1U] = {0};
    CHECK(!d1l_meshcore_trace_build_source(
        1U, 2U, 4U, oversized_path, 44U, &source));
    CHECK(d1l_meshcore_trace_build_source(
        1U, 2U, 8U, oversized_path, 21U, &source));
    CHECK(source.raw_len == 179U);
    CHECK(!d1l_meshcore_trace_build_source(
        1U, 2U, 8U, oversized_path, 22U, &source));

    const int8_t snrs[] = {-4, 8, 127};
    uint8_t raw[D1L_MESHCORE_MAX_RAW_PACKET] = {0};
    size_t raw_len = build_trace_frame(
        0x12345678U, 0xA1B2C3D4U, 0U, original_hashes, 3U, snrs, 1U,
        raw);
    CHECK(d1l_meshcore_trace_classify(raw, raw_len, &unchanged) ==
          D1L_MESHCORE_TRACE_FRAME_IN_FLIGHT);
    CHECK(!d1l_meshcore_trace_parse_terminal(raw, raw_len, &unchanged));
    CHECK(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);

    const uint8_t four_byte_hash[] = {0x11U, 0x12U, 0x13U, 0x14U};
    raw_len = build_trace_frame(
        1U, 2U, 2U, four_byte_hash, sizeof(four_byte_hash), snrs, 1U,
        raw);
    d1l_meshcore_trace_terminal_t four_byte_terminal = {0};
    CHECK(d1l_meshcore_trace_classify(raw, raw_len, &four_byte_terminal) ==
          D1L_MESHCORE_TRACE_FRAME_TERMINAL);
    CHECK(four_byte_terminal.path_hash_bytes == 4U);
    CHECK(four_byte_terminal.path_hops == 1U);
    CHECK(memcmp(four_byte_terminal.path_hashes, four_byte_hash,
                 sizeof(four_byte_hash)) == 0);

    const uint8_t eight_byte_hash[] = {
        0x21U, 0x22U, 0x23U, 0x24U,
        0x25U, 0x26U, 0x27U, 0x28U,
    };
    raw_len = build_trace_frame(
        1U, 2U, 3U, eight_byte_hash, sizeof(eight_byte_hash), snrs, 1U,
        raw);
    d1l_meshcore_trace_terminal_t eight_byte_terminal = {0};
    CHECK(d1l_meshcore_trace_classify(raw, raw_len, &eight_byte_terminal) ==
          D1L_MESHCORE_TRACE_FRAME_TERMINAL);
    CHECK(eight_byte_terminal.path_hash_bytes == 8U);
    CHECK(eight_byte_terminal.path_hops == 1U);
    CHECK(memcmp(eight_byte_terminal.path_hashes, eight_byte_hash,
                 sizeof(eight_byte_hash)) == 0);

    const uint8_t two_byte_hashes[] = {
        0x11U, 0x12U, 0x22U, 0x23U,
    };
    raw_len = build_trace_frame(
        1U, 2U, 1U, two_byte_hashes, sizeof(two_byte_hashes), snrs, 2U,
        raw);
    d1l_meshcore_trace_terminal_t two_byte_terminal = {0};
    CHECK(d1l_meshcore_trace_classify(raw, raw_len, &two_byte_terminal) ==
          D1L_MESHCORE_TRACE_FRAME_TERMINAL);
    CHECK(two_byte_terminal.path_hash_bytes == 2U);
    CHECK(two_byte_terminal.path_hops == 2U);
    CHECK(memcmp(two_byte_terminal.path_hashes, two_byte_hashes,
                 sizeof(two_byte_hashes)) == 0);
    CHECK(memcmp(two_byte_terminal.path_snrs_quarter_db, snrs, 2U) == 0);

    raw_len = build_trace_frame(
        1U, 2U, 0U, original_hashes, 0U, snrs, 1U, raw);
    CHECK(d1l_meshcore_trace_classify(raw, raw_len, &unchanged) ==
          D1L_MESHCORE_TRACE_FRAME_UNSUPPORTED);
    CHECK(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);

    raw_len = build_trace_frame(
        1U, 2U, 0U, original_hashes, 0U, snrs, 0U, raw);
    CHECK(d1l_meshcore_trace_classify(raw, raw_len, &unchanged) ==
          D1L_MESHCORE_TRACE_FRAME_UNSUPPORTED);
    CHECK(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);

    const int8_t four_snrs[] = {-4, 8, 12, 16};
    raw_len = build_trace_frame(
        1U, 2U, 0U, original_hashes, 3U, four_snrs, 4U, raw);
    CHECK(d1l_meshcore_trace_classify(raw, raw_len, &unchanged) ==
          D1L_MESHCORE_TRACE_FRAME_MALFORMED);
    CHECK(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);

    raw_len = build_terminal(
        0x12345678U, 0xA1B2C3D4U, original_hashes, snrs, 3U, raw);
    d1l_meshcore_trace_terminal_t terminal = {0};
    CHECK(d1l_meshcore_trace_classify(raw, raw_len, NULL) ==
          D1L_MESHCORE_TRACE_FRAME_TERMINAL);
    CHECK(d1l_meshcore_trace_parse_terminal(raw, raw_len, &terminal));
    CHECK(terminal.tag == 0x12345678U);
    CHECK(terminal.auth_code == 0xA1B2C3D4U);
    CHECK(terminal.path_hash_bytes == 1U);
    CHECK(terminal.path_hops == 3U);
    CHECK(memcmp(terminal.path_hashes, original_hashes, 3U) == 0);
    CHECK(memcmp(terminal.path_snrs_quarter_db, snrs, 3U) == 0);
    raw[2] = 0;
    raw[raw_len - 1U] = 0;
    CHECK(terminal.path_snrs_quarter_db[0] == -4);
    CHECK(terminal.path_hashes[2] == 0x33U);

    CHECK(!d1l_meshcore_trace_parse_terminal(source.raw, source.raw_len,
                                              &unchanged));
    CHECK(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);
    CHECK(!d1l_meshcore_trace_parse_terminal(NULL, raw_len, &unchanged));
    CHECK(!d1l_meshcore_trace_parse_terminal(raw, raw_len, NULL));

    const size_t canonical_len = build_terminal(
        1U, 2U, original_hashes, snrs, 3U, raw);
    raw[0] = (uint8_t)((D1L_MESHCORE_PAYLOAD_TRACE << 2U) |
                       D1L_MESHCORE_ROUTE_FLOOD);
    CHECK(!d1l_meshcore_trace_parse_terminal(raw, canonical_len, &unchanged));
    raw[0] = (uint8_t)((D1L_MESHCORE_PAYLOAD_TRACE << 2U) |
                       D1L_MESHCORE_ROUTE_DIRECT | 0x40U);
    CHECK(!d1l_meshcore_trace_parse_terminal(raw, canonical_len, &unchanged));
    raw[0] = (uint8_t)((D1L_MESHCORE_PAYLOAD_TEXT << 2U) |
                       D1L_MESHCORE_ROUTE_DIRECT);
    CHECK(!d1l_meshcore_trace_parse_terminal(raw, canonical_len, &unchanged));
    (void)build_terminal(1U, 2U, original_hashes, snrs, 3U, raw);
    raw[2U + 3U + 8U] = 0x04U;
    CHECK(d1l_meshcore_trace_classify(raw, canonical_len, &unchanged) ==
          D1L_MESHCORE_TRACE_FRAME_UNSUPPORTED);
    CHECK(!d1l_meshcore_trace_parse_terminal(raw, canonical_len, &unchanged));
    (void)build_terminal(1U, 2U, original_hashes, snrs, 3U, raw);
    raw[1] = 2U;
    CHECK(!d1l_meshcore_trace_parse_terminal(raw, canonical_len, &unchanged));
    CHECK(!d1l_meshcore_trace_parse_terminal(raw, 10U, &unchanged));

    raw_len = build_trace_frame(
        1U, 2U, 3U, oversized_path,
        D1L_MESHCORE_TRACE_MAX_PATH_BYTES + 1U, snrs, 0U, raw);
    CHECK(d1l_meshcore_trace_classify(raw, raw_len, &unchanged) ==
          D1L_MESHCORE_TRACE_FRAME_MALFORMED);
    CHECK(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);
    return 0;
}

static int test_tracker(void)
{
    CHECK(strcmp(d1l_meshcore_trace_outcome_name(
                     D1L_MESHCORE_TRACE_OUTCOME_NONE),
                 "none") == 0);
    CHECK(strcmp(d1l_meshcore_trace_outcome_name(
                     D1L_MESHCORE_TRACE_OUTCOME_PENDING),
                 "pending") == 0);
    CHECK(strcmp(d1l_meshcore_trace_outcome_name(
                     D1L_MESHCORE_TRACE_OUTCOME_MATCHED),
                 "matched") == 0);
    CHECK(strcmp(d1l_meshcore_trace_outcome_name(
                     D1L_MESHCORE_TRACE_OUTCOME_NO_RESPONSE),
                 "no_response") == 0);

    const uint8_t hashes[] = {0x10U, 0x20U};
    d1l_meshcore_trace_tracker_t invalid_tracker = {
        .attempt_valid = true,
        .attempt_outcome = D1L_MESHCORE_TRACE_OUTCOME_NONE,
    };
    CHECK(!d1l_meshcore_trace_tracker_begin(
        &invalid_tracker, 1U, 2U, 1U, hashes, 2U, 0U));

    uint8_t mutable_hashes[] = {0x10U, 0x20U};
    d1l_meshcore_trace_tracker_t tracker = {0};
    CHECK(d1l_meshcore_trace_tracker_begin(
        &tracker, 7U, 9U, 1U, mutable_hashes, 2U, 100U));
    mutable_hashes[0] = 0xFFU;
    CHECK(tracker.pending_path_hashes[0] == 0x10U);
    CHECK(tracker.attempt_valid);
    CHECK(tracker.attempt_outcome == D1L_MESHCORE_TRACE_OUTCOME_PENDING);
    CHECK(d1l_meshcore_trace_tracker_cooldown_remaining_ms(
              &tracker, 100U) == 0U);
    CHECK(!d1l_meshcore_trace_tracker_begin(
        &tracker, 8U, 10U, 1U, hashes, 2U, 101U));

    d1l_meshcore_trace_terminal_t terminal = {
        .tag = 8U,
        .auth_code = 9U,
        .path_hash_bytes = 1U,
        .path_hops = 2U,
        .path_hashes = {0x10U, 0x20U},
        .path_snrs_quarter_db = {-1, 2},
    };
    CHECK(d1l_meshcore_trace_tracker_consume(&tracker, &terminal, 110U) ==
          D1L_MESHCORE_TRACE_CORRELATION_UNMATCHED);
    CHECK(tracker.pending);
    terminal.tag = 7U;
    terminal.auth_code = 10U;
    CHECK(d1l_meshcore_trace_tracker_consume(&tracker, &terminal, 111U) ==
          D1L_MESHCORE_TRACE_CORRELATION_AUTH_MISMATCH);
    CHECK(tracker.pending);
    terminal.auth_code = 9U;
    terminal.path_hashes[1] = 0x21U;
    CHECK(d1l_meshcore_trace_tracker_consume(&tracker, &terminal, 112U) ==
          D1L_MESHCORE_TRACE_CORRELATION_PATH_MISMATCH);
    CHECK(tracker.pending);
    terminal.path_hashes[1] = 0x20U;
    terminal.path_hash_bytes = 2U;
    CHECK(d1l_meshcore_trace_tracker_consume(&tracker, &terminal, 112U) ==
          D1L_MESHCORE_TRACE_CORRELATION_PATH_MISMATCH);
    CHECK(tracker.pending);
    terminal.path_hash_bytes = 1U;
    CHECK(d1l_meshcore_trace_tracker_consume(&tracker, &terminal, 113U) ==
          D1L_MESHCORE_TRACE_CORRELATION_MATCHED);
    CHECK(!tracker.pending && tracker.completed);
    CHECK(tracker.attempt_valid);
    CHECK(tracker.attempt_outcome == D1L_MESHCORE_TRACE_OUTCOME_MATCHED);
    CHECK(tracker.attempt_outcome_at_ms == 113U);
    CHECK(d1l_meshcore_trace_tracker_cooldown_remaining_ms(
              &tracker, 114U) ==
          D1L_MESHCORE_TRACE_COOLDOWN_MS - 1U);
    terminal.path_hashes[0] = 0xEEU;
    terminal.path_snrs_quarter_db[0] = 99;
    CHECK(tracker.last_result.path_hashes[0] == 0x10U);
    CHECK(tracker.last_result.path_snrs_quarter_db[0] == -1);
    terminal.path_hashes[0] = 0x10U;
    CHECK(d1l_meshcore_trace_tracker_consume(&tracker, &terminal, 114U) ==
          D1L_MESHCORE_TRACE_CORRELATION_DUPLICATE);
    CHECK(d1l_meshcore_trace_tracker_consume(
              &tracker, &terminal,
              113U + D1L_MESHCORE_TRACE_DUPLICATE_WINDOW_MS) ==
          D1L_MESHCORE_TRACE_CORRELATION_UNMATCHED);
    CHECK(!d1l_meshcore_trace_tracker_begin(
        &tracker, 8U, 10U, 1U, hashes, 2U,
        113U + D1L_MESHCORE_TRACE_COOLDOWN_MS - 1U));
    CHECK(d1l_meshcore_trace_tracker_begin(
        &tracker, 8U, 10U, 1U, hashes, 2U,
        113U + D1L_MESHCORE_TRACE_COOLDOWN_MS));
    CHECK(d1l_meshcore_trace_tracker_cancel(&tracker, 8U, 10U));

    memset(&tracker, 0, sizeof(tracker));
    CHECK(d1l_meshcore_trace_tracker_begin(
        &tracker, 7U, 9U, 1U, hashes, 2U, 100U));
    CHECK(!d1l_meshcore_trace_tracker_expire_pending(
        &tracker, 100U + D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS - 1U));
    CHECK(d1l_meshcore_trace_tracker_consume(
              &tracker, &terminal,
              100U + D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS) ==
          D1L_MESHCORE_TRACE_CORRELATION_EXPIRED);
    CHECK(!tracker.pending && !tracker.completed);
    CHECK(tracker.attempt_valid);
    CHECK(tracker.attempt_outcome ==
          D1L_MESHCORE_TRACE_OUTCOME_NO_RESPONSE);
    CHECK(tracker.attempt_outcome_at_ms ==
          100U + D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS);
    CHECK(d1l_meshcore_trace_tracker_cooldown_remaining_ms(
              &tracker,
              100U + D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS) ==
          D1L_MESHCORE_TRACE_COOLDOWN_MS);
    CHECK(d1l_meshcore_trace_tracker_consume(
              &tracker, &terminal,
              101U + D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS) ==
          D1L_MESHCORE_TRACE_CORRELATION_EXPIRED);
    CHECK(!d1l_meshcore_trace_tracker_begin(
        &tracker, 8U, 10U, 1U, hashes, 2U,
        100U + D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS +
            D1L_MESHCORE_TRACE_COOLDOWN_MS - 1U));
    CHECK(d1l_meshcore_trace_tracker_begin(
        &tracker, 8U, 10U, 1U, hashes, 2U,
        100U + D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS +
            D1L_MESHCORE_TRACE_COOLDOWN_MS));
    CHECK(d1l_meshcore_trace_tracker_consume(
              &tracker, &terminal,
              101U + D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS +
                  D1L_MESHCORE_TRACE_COOLDOWN_MS) ==
          D1L_MESHCORE_TRACE_CORRELATION_EXPIRED);
    CHECK(tracker.pending && tracker.pending_tag == 8U);
    CHECK(d1l_meshcore_trace_tracker_cancel(&tracker, 8U, 10U));

    memset(&tracker, 0, sizeof(tracker));
    const uint32_t wrap_start = UINT32_MAX - 10U;
    CHECK(d1l_meshcore_trace_tracker_begin(
        &tracker, 1U, 2U, 1U, hashes, 2U, wrap_start));
    CHECK(!d1l_meshcore_trace_tracker_expire_pending(&tracker, 8U));
    CHECK(d1l_meshcore_trace_tracker_expire_pending(
        &tracker, wrap_start + D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS));
    CHECK(!tracker.pending);
    CHECK(tracker.attempt_outcome ==
          D1L_MESHCORE_TRACE_OUTCOME_NO_RESPONSE);
    CHECK(d1l_meshcore_trace_tracker_cooldown_remaining_ms(
              &tracker,
              wrap_start + D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS + 1U) ==
          D1L_MESHCORE_TRACE_COOLDOWN_MS - 1U);
    CHECK(!d1l_meshcore_trace_tracker_cancel(&tracker, 1U, 2U));

    CHECK(d1l_meshcore_trace_tracker_begin(
        &tracker, 3U, 4U, 1U, hashes, 2U, 500U));
    CHECK(!d1l_meshcore_trace_tracker_cancel(&tracker, 3U, 5U));
    CHECK(d1l_meshcore_trace_tracker_cancel(&tracker, 3U, 4U));
    CHECK(!tracker.pending);
    CHECK(!tracker.attempt_valid);
    CHECK(tracker.attempt_outcome == D1L_MESHCORE_TRACE_OUTCOME_NONE);

    const uint8_t two_byte_hashes[] = {
        0x10U, 0x11U, 0x20U, 0x21U,
    };
    memset(&tracker, 0, sizeof(tracker));
    CHECK(d1l_meshcore_trace_tracker_begin(
        &tracker, 5U, 6U, 2U, two_byte_hashes, 2U, 700U));
    d1l_meshcore_trace_terminal_t two_byte_terminal = {
        .tag = 5U,
        .auth_code = 6U,
        .path_hash_bytes = 2U,
        .path_hops = 2U,
        .path_hashes = {0x10U, 0x11U, 0x20U, 0x21U},
        .path_snrs_quarter_db = {-4, 8},
    };
    CHECK(d1l_meshcore_trace_tracker_consume(
              &tracker, &two_byte_terminal, 701U) ==
          D1L_MESHCORE_TRACE_CORRELATION_MATCHED);
    CHECK(tracker.last_result.path_hash_bytes == 2U);
    CHECK(memcmp(tracker.last_result.path_hashes, two_byte_hashes,
                 sizeof(two_byte_hashes)) == 0);

    const uint8_t four_byte_hashes[] = {
        0x10U, 0x11U, 0x12U, 0x13U,
        0x20U, 0x21U, 0x22U, 0x23U,
    };
    memset(&tracker, 0, sizeof(tracker));
    CHECK(d1l_meshcore_trace_tracker_begin(
        &tracker, 7U, 8U, 4U, four_byte_hashes, 2U, 800U));
    d1l_meshcore_trace_terminal_t four_byte_terminal = {
        .tag = 7U,
        .auth_code = 8U,
        .path_hash_bytes = 4U,
        .path_hops = 2U,
        .path_hashes = {
            0x10U, 0x11U, 0x12U, 0x13U,
            0x20U, 0x21U, 0x22U, 0x23U,
        },
        .path_snrs_quarter_db = {-4, 8},
    };
    CHECK(d1l_meshcore_trace_tracker_consume(
              &tracker, &four_byte_terminal, 801U) ==
          D1L_MESHCORE_TRACE_CORRELATION_MATCHED);
    CHECK(tracker.last_result.path_hash_bytes == 4U);
    CHECK(memcmp(tracker.last_result.path_hashes, four_byte_hashes,
                 sizeof(four_byte_hashes)) == 0);
    return 0;
}

int main(void)
{
    CHECK(test_contact_plan() == 0);
    CHECK(test_source_and_terminal() == 0);
    CHECK(test_tracker() == 0);
    puts("meshcore_trace_test: ok");
    return 0;
}
