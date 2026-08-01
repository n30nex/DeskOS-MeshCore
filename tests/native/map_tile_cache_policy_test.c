#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "map/map_tile_cache_policy.h"

static d1l_map_tile_cache_record_t record(
    uint32_t sequence,
    uint32_t size,
    uint8_t zoom,
    uint32_t x,
    uint32_t y)
{
    d1l_map_tile_cache_record_t value = {0};
    assert(d1l_map_tile_cache_record_init(
        sequence, zoom, x, y, size,
        UINT32_C(0x12340000) + sequence, &value));
    return value;
}

static void test_record_and_state_checksums_reject_damage(void)
{
    static const uint8_t payload[] = "interrupted-map-tile";
    const uint32_t whole_crc = d1l_map_tile_cache_crc32(
        payload, sizeof(payload) - 1U);
    uint32_t chunked_crc = d1l_map_tile_cache_crc32_update(
        0U, payload, 7U);
    chunked_crc = d1l_map_tile_cache_crc32_update(
        chunked_crc, &payload[7], sizeof(payload) - 1U - 7U);
    assert(whole_crc == chunked_crc);

    d1l_map_tile_cache_record_t original =
        record(7U, 65536U, 14U, 1234U, 5678U);
    uint8_t encoded_record[D1L_MAP_TILE_CACHE_RECORD_BYTES];
    assert(d1l_map_tile_cache_record_encode(
        &original, encoded_record));
    d1l_map_tile_cache_record_t decoded_record = {0};
    assert(d1l_map_tile_cache_record_decode(
        encoded_record, &decoded_record));
    assert(original.sequence == decoded_record.sequence);
    assert(original.size == decoded_record.size);
    assert(original.content_crc32 == decoded_record.content_crc32);
    assert(original.zoom == decoded_record.zoom);
    assert(!decoded_record.quarantined);
    assert(original.x == decoded_record.x);
    assert(original.y == decoded_record.y);
    encoded_record[20] ^= 1U;
    assert(!d1l_map_tile_cache_record_decode(
        encoded_record, &decoded_record));

    d1l_map_tile_cache_state_t state = {0};
    d1l_map_tile_cache_state_init(&state);
    uint8_t encoded_state[D1L_MAP_TILE_CACHE_STATE_BYTES];
    assert(d1l_map_tile_cache_state_encode(&state, encoded_state));
    d1l_map_tile_cache_state_t decoded_state = {0};
    assert(d1l_map_tile_cache_state_decode(
        encoded_state, &decoded_state));
    assert(decoded_state.next_sequence == 1U);
    encoded_state[24] ^= 1U;
    assert(!d1l_map_tile_cache_state_decode(
        encoded_state, &decoded_state));
}

static void test_budget_evicts_oldest_records_only(void)
{
    d1l_map_tile_cache_state_t state = {0};
    d1l_map_tile_cache_state_init(&state);
    const d1l_map_tile_cache_record_t first =
        record(1U, 60U, 8U, 1U, 2U);
    const d1l_map_tile_cache_record_t second =
        record(2U, 30U, 9U, 3U, 4U);
    assert(d1l_map_tile_cache_state_note_commit(&state, &first));
    assert(d1l_map_tile_cache_state_note_commit(&state, &second));
    assert(state.live_bytes == 90U);
    assert(!d1l_map_tile_cache_state_has_room(&state, 100U, 20U));

    assert(d1l_map_tile_cache_state_note_evict(&state, &first));
    assert(state.head_offset == D1L_MAP_TILE_CACHE_RECORD_BYTES);
    assert(state.live_bytes == 30U);
    assert(d1l_map_tile_cache_state_has_room(&state, 100U, 20U));

    const d1l_map_tile_cache_record_t third =
        record(3U, 20U, 10U, 5U, 6U);
    assert(d1l_map_tile_cache_state_note_commit(&state, &third));
    assert(state.live_bytes == 50U);
    assert(state.next_sequence == 4U);
}

static void test_invalid_coordinates_and_overflow_fail_closed(void)
{
    d1l_map_tile_cache_record_t invalid = {0};
    assert(!d1l_map_tile_cache_record_init(
        1U, 8U, 256U, 0U, 1U, 0U, &invalid));
    assert(!d1l_map_tile_cache_record_init(
        1U, 19U, 0U, 0U, 1U, 0U, &invalid));

    d1l_map_tile_cache_state_t state = {0};
    d1l_map_tile_cache_state_init(&state);
    state.live_bytes = UINT64_MAX;
    const d1l_map_tile_cache_record_t value =
        record(1U, 1U, 8U, 0U, 0U);
    assert(!d1l_map_tile_cache_state_note_commit(&state, &value));
    assert(!d1l_map_tile_cache_state_has_room(&state, 10U, 1U));
}

static void test_quarantine_keeps_unknown_bytes_charged(void)
{
    d1l_map_tile_cache_state_t state = {0};
    d1l_map_tile_cache_state_init(&state);
    const d1l_map_tile_cache_record_t first =
        record(1U, 60U, 8U, 1U, 2U);
    const d1l_map_tile_cache_record_t second =
        record(2U, 30U, 9U, 3U, 4U);
    assert(d1l_map_tile_cache_state_note_commit(&state, &first));
    assert(d1l_map_tile_cache_state_note_commit(&state, &second));

    assert(d1l_map_tile_cache_state_quarantine_head(&state, 0U));
    assert(state.head_offset == D1L_MAP_TILE_CACHE_RECORD_BYTES);
    assert(state.live_bytes == 90U);
    assert(!d1l_map_tile_cache_state_has_room(&state, 100U, 20U));

    assert(d1l_map_tile_cache_state_note_evict(&state, &second));
    assert(state.head_offset == state.tail_offset);
    assert(state.live_bytes == 60U);
    assert(d1l_map_tile_cache_state_has_room(&state, 100U, 20U));
    assert(!d1l_map_tile_cache_state_quarantine_head(&state, 0U));

    d1l_map_tile_cache_state_init(&state);
    assert(d1l_map_tile_cache_state_note_commit(&state, &first));
    assert(d1l_map_tile_cache_state_quarantine_head(&state, 100U));
    assert(state.head_offset == state.tail_offset);
    assert(state.live_bytes == 160U);
}

static void test_quarantine_record_is_checksummed_and_reconstructible(void)
{
    d1l_map_tile_cache_record_t quarantine = {0};
    assert(d1l_map_tile_cache_record_init_quarantine(
        7U, 196U * 1024U, &quarantine));
    assert(quarantine.quarantined);
    assert(quarantine.size == 196U * 1024U);

    uint8_t encoded[D1L_MAP_TILE_CACHE_RECORD_BYTES];
    assert(d1l_map_tile_cache_record_encode(&quarantine, encoded));
    assert(encoded[9] == 1U);
    d1l_map_tile_cache_record_t decoded = {0};
    assert(d1l_map_tile_cache_record_decode(encoded, &decoded));
    assert(decoded.quarantined);
    assert(decoded.sequence == quarantine.sequence);
    assert(decoded.size == quarantine.size);
    encoded[9] = 2U;
    const uint32_t unknown_flag_crc =
        d1l_map_tile_cache_crc32(encoded, 28U);
    encoded[28] = (uint8_t)unknown_flag_crc;
    encoded[29] = (uint8_t)(unknown_flag_crc >> 8U);
    encoded[30] = (uint8_t)(unknown_flag_crc >> 16U);
    encoded[31] = (uint8_t)(unknown_flag_crc >> 24U);
    assert(!d1l_map_tile_cache_record_decode(encoded, &decoded));
}

static void test_every_atomic_interruption_window_has_one_safe_plan(void)
{
    d1l_map_tile_cache_recovery_plan_t plan = {0};
    assert(d1l_map_tile_cache_recovery_plan(
        true, false, true, false, &plan));
    assert(!plan.rename_tile);
    assert(!plan.rename_metadata);

    assert(d1l_map_tile_cache_recovery_plan(
        false, true, false, true, &plan));
    assert(plan.rename_tile);
    assert(plan.rename_metadata);

    assert(d1l_map_tile_cache_recovery_plan(
        true, false, false, true, &plan));
    assert(!plan.rename_tile);
    assert(plan.rename_metadata);

    assert(d1l_map_tile_cache_recovery_plan(
        false, true, true, false, &plan));
    assert(plan.rename_tile);
    assert(!plan.rename_metadata);

    assert(!d1l_map_tile_cache_recovery_plan(
        false, false, true, false, &plan));
    assert(!d1l_map_tile_cache_recovery_plan(
        true, false, false, false, &plan));
}

static void test_journal_repair_keeps_only_valid_complete_prefix(void)
{
    d1l_map_tile_cache_journal_repair_plan_t plan = {0};

    assert(d1l_map_tile_cache_journal_repair_plan(
        69U, 32U, 64U, &plan));
    assert(plan.rebuild);
    assert(plan.valid_prefix_bytes == 64U);

    assert(d1l_map_tile_cache_journal_repair_plan(
        64U, 32U, 32U, &plan));
    assert(plan.rebuild);
    assert(plan.valid_prefix_bytes == 32U);

    assert(d1l_map_tile_cache_journal_repair_plan(
        64U, 32U, 64U, &plan));
    assert(!plan.rebuild);
    assert(plan.valid_prefix_bytes == 64U);

    assert(!d1l_map_tile_cache_journal_repair_plan(
        64U, 64U, 32U, &plan));
    assert(!d1l_map_tile_cache_journal_repair_plan(
        64U, 32U, 33U, &plan));
}

int main(void)
{
    test_record_and_state_checksums_reject_damage();
    test_budget_evicts_oldest_records_only();
    test_invalid_coordinates_and_overflow_fail_closed();
    test_quarantine_keeps_unknown_bytes_charged();
    test_quarantine_record_is_checksummed_and_reconstructible();
    test_every_atomic_interruption_window_has_one_safe_plan();
    test_journal_repair_keeps_only_valid_complete_prefix();
    return 0;
}
