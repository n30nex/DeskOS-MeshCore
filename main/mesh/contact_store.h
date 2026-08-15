#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "mesh/meshcore_path_state.h"
#include "mesh/node_store.h"

#define D1L_CONTACT_STORE_CAPACITY 16U
#define D1L_CONTACT_ALIAS_LEN 32U
#define D1L_CONTACT_OUT_PATH_MAX 64U
#define D1L_CONTACT_EXPORT_URI_LEN 224U
#define D1L_CONTACT_PATH_PERSIST_MIN_INTERVAL_MS 1000U

typedef enum {
    D1L_CONTACT_VERIFICATION_NONE = 0,
    D1L_CONTACT_VERIFICATION_SIGNED_ADVERT = 1,
    D1L_CONTACT_VERIFICATION_URI_IMPORT = 2,
    D1L_CONTACT_VERIFICATION_MIGRATED_SIGNED_ADVERT = 3,
} d1l_contact_verification_source_t;

typedef struct {
    uint32_t seq;
    uint32_t created_ms;
    uint32_t updated_ms;
    uint32_t out_path_updated_ms;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char public_key_hex[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    char alias[D1L_CONTACT_ALIAS_LEN];
    char heard_name[D1L_HEARD_NODE_NAME_LEN];
    char type[D1L_NODE_TYPE_LEN];
    int last_rssi_dbm;
    int last_snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool out_path_valid;
    uint8_t out_path_len;
    uint8_t out_path[D1L_CONTACT_OUT_PATH_MAX];
    bool favorite;
    bool muted;
    uint8_t verification_source;
    uint32_t verified_at_ms;
    uint32_t signed_advert_timestamp;
    uint32_t last_heard_ms;
    d1l_meshcore_path_state_t out_path_state;
} d1l_contact_entry_t;

typedef struct {
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    size_t count;
    size_t capacity;
    /* Widened for shared retained-worker telemetry. The contact store's
     * 32-bit backing counter saturates and rejects further mutations instead
     * of wrapping, so equality can never ABA within a boot. */
    uint64_t persistence_revision;
    uint32_t persistence_commit_count;
    uint32_t persistence_coalesced_count;
    uint32_t persistence_fail_count;
    uint32_t sd_backend_generation;
    esp_err_t persistence_last_error;
    bool persistence_dirty;
    bool loaded;
    bool sd_primary_required;
    bool sd_primary_reconcile_pending;
} d1l_contact_store_stats_t;

typedef enum {
    D1L_CONTACT_VERIFIED_ADVERT_NONE = 0,
    D1L_CONTACT_VERIFIED_ADVERT_CREATED,
    D1L_CONTACT_VERIFIED_ADVERT_UPDATED,
    D1L_CONTACT_VERIFIED_ADVERT_PROMOTED_PLACEHOLDER,
    D1L_CONTACT_VERIFIED_ADVERT_COLLISION,
    D1L_CONTACT_VERIFIED_ADVERT_FULL,
} d1l_contact_verified_advert_result_t;

typedef enum {
    D1L_CONTACT_IMPORT_NONE = 0,
    D1L_CONTACT_IMPORT_CREATED,
    D1L_CONTACT_IMPORT_UPDATED,
    D1L_CONTACT_IMPORT_PROMOTED_PLACEHOLDER,
    D1L_CONTACT_IMPORT_COLLISION,
    D1L_CONTACT_IMPORT_ROLE_CONFLICT,
    D1L_CONTACT_IMPORT_FULL,
} d1l_contact_import_result_t;

esp_err_t d1l_contact_store_init(void);
esp_err_t d1l_contact_store_clear(void);
esp_err_t d1l_contact_store_flush(void);
esp_err_t d1l_contact_store_flush_if_due(void);
esp_err_t d1l_contact_store_upsert_from_node(const char *fingerprint, const char *alias,
                                             const d1l_node_entry_t *heard_node);
/*
 * Stores identity metadata only after the caller has verified the signed
 * advert. Exact full public keys are authoritative; the short fingerprint is
 * only an index hint and can never authorize replacement or eviction. Ambient
 * advert refreshes become readable immediately and are durably coalesced by
 * the retained-store worker; explicit user edits remain synchronous.
 */
esp_err_t d1l_contact_store_upsert_verified_advert(
    const char *fingerprint, const d1l_node_entry_t *verified_node,
    d1l_contact_verified_advert_result_t *out_result,
    d1l_contact_entry_t *out_entry);
/*
 * Validates and atomically imports a complete MeshCore contact URI. The full
 * public key is authoritative, duplicate merges preserve local preferences,
 * and a URI can never overwrite conflicting signed-advert role truth.
 */
esp_err_t d1l_contact_store_import_uri(
    const char *uri, size_t uri_len, d1l_contact_import_result_t *out_result,
    d1l_contact_entry_t *out_entry);
esp_err_t d1l_contact_store_update_path(const char *fingerprint, const uint8_t *path,
                                        uint8_t path_len);
esp_err_t d1l_contact_store_update_path_from_source(
    const char *fingerprint, const uint8_t *path, uint8_t path_len,
    d1l_meshcore_path_source_t source, d1l_contact_entry_t *out_entry);
/* Forget only this contact's learned outbound route. Identity, preferences,
 * messages, and every other contact remain untouched. */
esp_err_t d1l_contact_store_reset_path(
    const char *fingerprint, d1l_contact_entry_t *out_entry);
esp_err_t d1l_contact_store_prepare_path_route(
    const char *fingerprint, uint32_t now_ms, d1l_contact_entry_t *out_entry,
    bool *out_expired);
esp_err_t d1l_contact_store_note_path_result(
    const char *fingerprint, uint32_t expected_generation, bool success,
    uint32_t now_ms, d1l_contact_entry_t *out_entry,
    d1l_meshcore_path_result_t *out_result);
esp_err_t d1l_contact_store_set_flags(const char *fingerprint, bool favorite, bool muted,
                                      d1l_contact_entry_t *out_entry);
esp_err_t d1l_contact_store_rename(const char *fingerprint, const char *alias,
                                   d1l_contact_entry_t *out_entry);
esp_err_t d1l_contact_store_delete(const char *fingerprint, d1l_contact_entry_t *out_entry);
uint8_t d1l_contact_store_meshcore_type_id(const char *type);
const char *d1l_contact_store_verification_source_name(uint8_t source);
bool d1l_contact_store_has_export_key(const d1l_contact_entry_t *entry);
bool d1l_contact_store_is_canonical(const d1l_contact_entry_t *entry);
bool d1l_contact_store_can_dm(const d1l_contact_entry_t *entry);
bool d1l_contact_store_can_path_probe(const d1l_contact_entry_t *entry);
bool d1l_contact_store_can_admin(const d1l_contact_entry_t *entry);
esp_err_t d1l_contact_store_export_uri(const d1l_contact_entry_t *entry, char *dest,
                                       size_t dest_size);
d1l_contact_store_stats_t d1l_contact_store_stats(void);
bool d1l_contact_store_find_by_fingerprint(const char *fingerprint, d1l_contact_entry_t *out_entry);
bool d1l_contact_store_find_by_public_key(const char *public_key_hex,
                                          d1l_contact_entry_t *out_entry);
size_t d1l_contact_store_copy_recent(d1l_contact_entry_t *out_entries, size_t max_entries);

#ifdef D1L_CONTACT_STORE_TEST_HOOKS
esp_err_t d1l_contact_store_test_set_persistence_revision(uint32_t revision);
#endif
