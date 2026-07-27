#include "map_tile_cache_policy.h"

#include <limits.h>
#include <string.h>

#define D1L_MAP_TILE_CACHE_RECORD_MAGIC UINT32_C(0x31454c54)
#define D1L_MAP_TILE_CACHE_STATE_MAGIC UINT32_C(0x3154534d)
#define D1L_MAP_TILE_CACHE_MAX_ZOOM 18U

static void put_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static uint32_t get_u32(const uint8_t *input)
{
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8U) |
           ((uint32_t)input[2] << 16U) |
           ((uint32_t)input[3] << 24U);
}

static void put_u64(uint8_t *output, uint64_t value)
{
    put_u32(output, (uint32_t)value);
    put_u32(&output[4], (uint32_t)(value >> 32U));
}

static uint64_t get_u64(const uint8_t *input)
{
    return (uint64_t)get_u32(input) |
           ((uint64_t)get_u32(&input[4]) << 32U);
}

uint32_t d1l_map_tile_cache_crc32_update(
    uint32_t crc,
    const uint8_t *data,
    size_t length)
{
    if (!data && length > 0U) {
        return 0U;
    }
    crc = ~crc;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^
                  (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

uint32_t d1l_map_tile_cache_crc32(const uint8_t *data, size_t length)
{
    return d1l_map_tile_cache_crc32_update(0U, data, length);
}

void d1l_map_tile_cache_state_init(d1l_map_tile_cache_state_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->next_sequence = 1U;
}

static bool state_valid(const d1l_map_tile_cache_state_t *state)
{
    return state &&
           state->next_sequence > 0U &&
           state->head_offset <= state->tail_offset &&
           state->head_offset % D1L_MAP_TILE_CACHE_RECORD_BYTES == 0U &&
           state->tail_offset % D1L_MAP_TILE_CACHE_RECORD_BYTES == 0U;
}

bool d1l_map_tile_cache_state_encode(
    const d1l_map_tile_cache_state_t *state,
    uint8_t output[D1L_MAP_TILE_CACHE_STATE_BYTES])
{
    if (!state_valid(state) || !output) {
        return false;
    }
    memset(output, 0, D1L_MAP_TILE_CACHE_STATE_BYTES);
    put_u32(&output[0], D1L_MAP_TILE_CACHE_STATE_MAGIC);
    put_u32(&output[4], D1L_MAP_TILE_CACHE_SCHEMA);
    put_u32(&output[8], state->head_offset);
    put_u32(&output[12], state->tail_offset);
    put_u32(&output[16], state->next_sequence);
    put_u64(&output[24], state->live_bytes);
    put_u32(
        &output[32],
        d1l_map_tile_cache_crc32(output, 32U));
    return true;
}

bool d1l_map_tile_cache_state_decode(
    const uint8_t input[D1L_MAP_TILE_CACHE_STATE_BYTES],
    d1l_map_tile_cache_state_t *state)
{
    if (!input || !state ||
        get_u32(&input[0]) != D1L_MAP_TILE_CACHE_STATE_MAGIC ||
        get_u32(&input[4]) != D1L_MAP_TILE_CACHE_SCHEMA ||
        get_u32(&input[32]) !=
            d1l_map_tile_cache_crc32(input, 32U)) {
        return false;
    }
    d1l_map_tile_cache_state_t decoded = {
        .head_offset = get_u32(&input[8]),
        .tail_offset = get_u32(&input[12]),
        .next_sequence = get_u32(&input[16]),
        .live_bytes = get_u64(&input[24]),
    };
    if (!state_valid(&decoded)) {
        return false;
    }
    *state = decoded;
    return true;
}

static bool record_valid(const d1l_map_tile_cache_record_t *record)
{
    if (!record || record->sequence == 0U ||
        record->size == 0U ||
        record->zoom > D1L_MAP_TILE_CACHE_MAX_ZOOM) {
        return false;
    }
    const uint32_t limit = 1UL << record->zoom;
    return record->x < limit && record->y < limit;
}

bool d1l_map_tile_cache_record_init(
    uint32_t sequence,
    uint8_t zoom,
    uint32_t x,
    uint32_t y,
    uint32_t size,
    uint32_t content_crc32,
    d1l_map_tile_cache_record_t *record)
{
    if (!record) {
        return false;
    }
    const d1l_map_tile_cache_record_t candidate = {
        .sequence = sequence,
        .size = size,
        .content_crc32 = content_crc32,
        .zoom = zoom,
        .x = x,
        .y = y,
    };
    if (!record_valid(&candidate)) {
        return false;
    }
    *record = candidate;
    return true;
}

bool d1l_map_tile_cache_record_encode(
    const d1l_map_tile_cache_record_t *record,
    uint8_t output[D1L_MAP_TILE_CACHE_RECORD_BYTES])
{
    if (!record_valid(record) || !output) {
        return false;
    }
    memset(output, 0, D1L_MAP_TILE_CACHE_RECORD_BYTES);
    put_u32(&output[0], D1L_MAP_TILE_CACHE_RECORD_MAGIC);
    put_u32(&output[4], record->sequence);
    output[8] = record->zoom;
    put_u32(&output[12], record->x);
    put_u32(&output[16], record->y);
    put_u32(&output[20], record->size);
    put_u32(&output[24], record->content_crc32);
    put_u32(
        &output[28],
        d1l_map_tile_cache_crc32(output, 28U));
    return true;
}

bool d1l_map_tile_cache_record_decode(
    const uint8_t input[D1L_MAP_TILE_CACHE_RECORD_BYTES],
    d1l_map_tile_cache_record_t *record)
{
    if (!input || !record ||
        get_u32(&input[0]) != D1L_MAP_TILE_CACHE_RECORD_MAGIC ||
        get_u32(&input[28]) !=
            d1l_map_tile_cache_crc32(input, 28U) ||
        input[9] != 0U || input[10] != 0U || input[11] != 0U) {
        return false;
    }
    d1l_map_tile_cache_record_t decoded = {
        .sequence = get_u32(&input[4]),
        .zoom = input[8],
        .x = get_u32(&input[12]),
        .y = get_u32(&input[16]),
        .size = get_u32(&input[20]),
        .content_crc32 = get_u32(&input[24]),
    };
    if (!record_valid(&decoded)) {
        return false;
    }
    *record = decoded;
    return true;
}

bool d1l_map_tile_cache_state_has_room(
    const d1l_map_tile_cache_state_t *state,
    uint64_t budget_bytes,
    uint64_t required_bytes)
{
    return state_valid(state) &&
           required_bytes > 0U &&
           required_bytes <= budget_bytes &&
           state->live_bytes <= budget_bytes - required_bytes;
}

bool d1l_map_tile_cache_state_note_commit(
    d1l_map_tile_cache_state_t *state,
    const d1l_map_tile_cache_record_t *record)
{
    if (!state_valid(state) || !record_valid(record) ||
        record->sequence != state->next_sequence ||
        state->tail_offset >
            UINT32_MAX - D1L_MAP_TILE_CACHE_RECORD_BYTES ||
        state->live_bytes > UINT64_MAX - record->size) {
        return false;
    }
    state->tail_offset += D1L_MAP_TILE_CACHE_RECORD_BYTES;
    state->live_bytes += record->size;
    state->next_sequence =
        record->sequence == UINT32_MAX ? 1U : record->sequence + 1U;
    return true;
}

bool d1l_map_tile_cache_state_note_evict(
    d1l_map_tile_cache_state_t *state,
    const d1l_map_tile_cache_record_t *oldest_record)
{
    if (!state_valid(state) || !record_valid(oldest_record) ||
        state->head_offset >= state->tail_offset ||
        state->live_bytes < oldest_record->size) {
        return false;
    }
    state->head_offset += D1L_MAP_TILE_CACHE_RECORD_BYTES;
    state->live_bytes -= oldest_record->size;
    return true;
}

bool d1l_map_tile_cache_recovery_plan(
    bool final_tile_valid,
    bool temporary_tile_valid,
    bool final_metadata_matches,
    bool temporary_metadata_matches,
    d1l_map_tile_cache_recovery_plan_t *plan)
{
    if (!plan ||
        (!final_tile_valid && !temporary_tile_valid) ||
        (!final_metadata_matches && !temporary_metadata_matches)) {
        return false;
    }
    *plan = (d1l_map_tile_cache_recovery_plan_t) {
        .rename_tile = !final_tile_valid,
        .rename_metadata = !final_metadata_matches,
    };
    return true;
}
