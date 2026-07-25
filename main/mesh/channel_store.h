#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define D1L_CHANNEL_STORE_CAPACITY 8U
#define D1L_CHANNEL_NAME_LEN 33U
#define D1L_CHANNEL_SECRET_MAX_LEN 32U
#define D1L_CHANNEL_SECRET_128_LEN 16U
#define D1L_CHANNEL_SECRET_256_LEN 32U
#define D1L_CHANNEL_SHARE_URI_LEN 256U
#define D1L_CHANNEL_PUBLIC_ID UINT64_C(1)
#define D1L_CHANNEL_RETAINED_ROW_CAPACITY 16U

typedef enum {
    D1L_CHANNEL_SOURCE_BUILTIN = 1,
    D1L_CHANNEL_SOURCE_MANUAL = 2,
    D1L_CHANNEL_SOURCE_URI_IMPORT = 3,
    D1L_CHANNEL_SOURCE_MIGRATED = 4,
} d1l_channel_source_t;

typedef enum {
    D1L_CHANNEL_MUTATION_NONE = 0,
    D1L_CHANNEL_MUTATION_CREATED,
    D1L_CHANNEL_MUTATION_UPDATED,
    D1L_CHANNEL_MUTATION_REMOVED,
    D1L_CHANNEL_MUTATION_EXISTS,
    D1L_CHANNEL_MUTATION_NAME_COLLISION,
    D1L_CHANNEL_MUTATION_SECRET_COLLISION,
    D1L_CHANNEL_MUTATION_FULL,
    D1L_CHANNEL_MUTATION_PROTECTED,
} d1l_channel_mutation_result_t;

/* Redacted application-facing channel metadata. Secret material is never
 * returned by list/find APIs and therefore cannot enter ordinary UI, logs, or
 * diagnostics by accident. */
typedef struct {
    uint64_t channel_id;
    uint64_t history_key;
    uint32_t created_ms;
    uint32_t updated_ms;
    uint32_t imported_at_ms;
    uint32_t newest_message_seq;
    uint32_t read_through_seq;
    uint32_t unread_count;
    char name[D1L_CHANNEL_NAME_LEN];
    uint8_t channel_hash;
    uint8_t source;
    bool enabled;
    bool is_default;
} d1l_channel_info_t;

/* Deliberately separate, secret-bearing protocol view. Only the single Mesh
 * runtime owner and explicit share/export flow should request this shape. */
typedef struct {
    uint64_t channel_id;
    uint8_t channel_hash;
    uint8_t secret_len;
    uint8_t secret[D1L_CHANNEL_SECRET_MAX_LEN];
} d1l_channel_protocol_key_t;

typedef struct {
    uint64_t lineage;
    uint64_t generation;
    uint64_t next_channel_id;
    uint64_t message_clear_lineage;
    uint32_t revision;
    uint32_t total_mutations;
    uint32_t message_epoch;
    uint32_t message_next_seq;
    size_t count;
    size_t capacity;
    esp_err_t load_status;
    bool loaded;
} d1l_channel_store_stats_t;

/* Secret-free exact-channel projection of the bounded retained message ring.
 * received is true only for an admitted RX text row. */
typedef struct {
    uint64_t channel_id;
    uint32_t message_seq;
    bool received;
} d1l_channel_retained_row_t;

typedef struct {
    uint32_t epoch;
    uint32_t next_seq;
    uint64_t clear_lineage;
} d1l_channel_message_generation_t;

esp_err_t d1l_channel_store_init(void);
esp_err_t d1l_channel_store_reset(void);

esp_err_t d1l_channel_store_add(
    const char *name, const uint8_t *secret, uint8_t secret_len,
    bool enabled, bool make_default, d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_info);
esp_err_t d1l_channel_store_import_uri(
    const char *uri, size_t uri_len,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_info);
esp_err_t d1l_channel_store_update(
    uint64_t channel_id, const char *name, bool enabled, bool make_default,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_info);
esp_err_t d1l_channel_store_select(
    uint64_t channel_id, d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_info);
esp_err_t d1l_channel_store_remove(
    uint64_t channel_id, d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_info);

esp_err_t d1l_channel_store_note_message(uint64_t channel_id,
                                         uint32_t message_seq,
                                         bool unread);
esp_err_t d1l_channel_store_mark_all_read(uint64_t channel_id);
/* Recomputes retained unread counts for every configured channel in one
 * persisted mutation. Cached note_message increments are never trusted as the
 * app-facing source after eviction, persistence failure, or reboot. */
esp_err_t d1l_channel_store_reconcile_retained_rows(
    const d1l_channel_retained_row_t *rows, size_t row_count,
    const d1l_channel_message_generation_t *message_generation);

d1l_channel_store_stats_t d1l_channel_store_stats(void);
/* Copies one coherent, redacted metadata generation under the store lock. */
esp_err_t d1l_channel_store_snapshot(
    d1l_channel_info_t *out_channels, size_t max_channels,
    size_t *out_count, uint64_t *out_active_channel_id,
    d1l_channel_store_stats_t *out_stats);
size_t d1l_channel_store_copy(d1l_channel_info_t *out_channels,
                              size_t max_channels);
bool d1l_channel_store_find(uint64_t channel_id,
                            d1l_channel_info_t *out_info);
bool d1l_channel_store_find_default(d1l_channel_info_t *out_info);

esp_err_t d1l_channel_store_copy_protocol_key(
    uint64_t channel_id, d1l_channel_protocol_key_t *out_key);
size_t d1l_channel_store_copy_hash_matches(
    uint8_t channel_hash, d1l_channel_protocol_key_t *out_keys,
    size_t max_keys);
/* RX-facing secret lookup is release-profile aware. Core 1.0 exposes only
 * the built-in Public key even when retained predecessor data still contains
 * enabled private channels. Development/full-feature profiles preserve the
 * all-match collision scan. */
size_t d1l_channel_store_copy_rx_hash_matches(
    uint8_t channel_hash, d1l_channel_protocol_key_t *out_keys,
    size_t max_keys);
/* Fails closed with ESP_ERR_INVALID_STATE when a one-byte protocol hash maps
 * to more than one enabled secret. */
esp_err_t d1l_channel_store_find_unique_hash(
    uint8_t channel_hash, d1l_channel_protocol_key_t *out_key);

/* This is the only normal API that emits secret material. The pinned official
 * MeshCore share/QR format accepts exactly a 16-byte (32-hex) secret; internal
 * 32-byte protocol keys are deliberately non-shareable here. Callers must
 * treat the returned URI as sensitive and must never log it. */
esp_err_t d1l_channel_store_export_share_uri(uint64_t channel_id, char *dest,
                                             size_t dest_size);

esp_err_t d1l_channel_store_protocol_hash(const uint8_t *secret,
                                          uint8_t secret_len,
                                          uint8_t *out_hash);
