#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define D1L_MAP_TILE_CACHE_RECORD_BYTES 32U
#define D1L_MAP_TILE_CACHE_STATE_BYTES 36U
#define D1L_MAP_TILE_CACHE_SCHEMA 1U

typedef struct {
    uint32_t sequence;
    uint32_t size;
    uint32_t content_crc32;
    uint8_t zoom;
    bool quarantined;
    uint32_t x;
    uint32_t y;
} d1l_map_tile_cache_record_t;

/* Quarantine uses reserved byte 9. Future record meanings require new magic. */

typedef struct {
    uint32_t head_offset;
    uint32_t tail_offset;
    uint32_t next_sequence;
    uint64_t live_bytes;
} d1l_map_tile_cache_state_t;

typedef struct {
    bool rename_tile;
    bool rename_metadata;
} d1l_map_tile_cache_recovery_plan_t;

typedef struct {
    uint32_t valid_prefix_bytes;
    bool rebuild;
} d1l_map_tile_cache_journal_repair_plan_t;

uint32_t d1l_map_tile_cache_crc32(const uint8_t *data, size_t length);
uint32_t d1l_map_tile_cache_crc32_update(
    uint32_t crc,
    const uint8_t *data,
    size_t length);

void d1l_map_tile_cache_state_init(d1l_map_tile_cache_state_t *state);
bool d1l_map_tile_cache_state_encode(
    const d1l_map_tile_cache_state_t *state,
    uint8_t output[D1L_MAP_TILE_CACHE_STATE_BYTES]);
bool d1l_map_tile_cache_state_decode(
    const uint8_t input[D1L_MAP_TILE_CACHE_STATE_BYTES],
    d1l_map_tile_cache_state_t *state);

bool d1l_map_tile_cache_record_init(
    uint32_t sequence,
    uint8_t zoom,
    uint32_t x,
    uint32_t y,
    uint32_t size,
    uint32_t content_crc32,
    d1l_map_tile_cache_record_t *record);
bool d1l_map_tile_cache_record_init_quarantine(
    uint32_t sequence,
    uint32_t charged_bytes,
    d1l_map_tile_cache_record_t *record);
bool d1l_map_tile_cache_record_encode(
    const d1l_map_tile_cache_record_t *record,
    uint8_t output[D1L_MAP_TILE_CACHE_RECORD_BYTES]);
bool d1l_map_tile_cache_record_decode(
    const uint8_t input[D1L_MAP_TILE_CACHE_RECORD_BYTES],
    d1l_map_tile_cache_record_t *record);

bool d1l_map_tile_cache_state_has_room(
    const d1l_map_tile_cache_state_t *state,
    uint64_t budget_bytes,
    uint64_t required_bytes);
bool d1l_map_tile_cache_state_note_commit(
    d1l_map_tile_cache_state_t *state,
    const d1l_map_tile_cache_record_t *record);
bool d1l_map_tile_cache_state_note_evict(
    d1l_map_tile_cache_state_t *state,
    const d1l_map_tile_cache_record_t *oldest_record);
bool d1l_map_tile_cache_state_quarantine_head(
    d1l_map_tile_cache_state_t *state);
bool d1l_map_tile_cache_recovery_plan(
    bool final_tile_valid,
    bool temporary_tile_valid,
    bool final_metadata_matches,
    bool temporary_metadata_matches,
    d1l_map_tile_cache_recovery_plan_t *plan);
bool d1l_map_tile_cache_journal_repair_plan(
    uint32_t file_size,
    uint32_t committed_tail_offset,
    uint32_t valid_complete_prefix_bytes,
    d1l_map_tile_cache_journal_repair_plan_t *plan);
