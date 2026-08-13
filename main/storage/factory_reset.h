#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT 18U
#define D1L_FACTORY_RESET_SD_STORE_COUNT 7U
#define D1L_FACTORY_RESET_SD_MEDIA_MARKER_SCHEMA 1U
#define D1L_FACTORY_RESET_INVENTORY_COUNT 41U
#define D1L_FACTORY_RESET_RAW_MARKER_COUNT 2U
#define D1L_FACTORY_RESET_RAW_MARKER_BYTES 16U
#define D1L_FACTORY_RESET_DOMAIN_NAME_LEN 32U

typedef enum {
    D1L_FACTORY_RESET_PARTITION_DEFAULT = 0,
    D1L_FACTORY_RESET_PARTITION_RETAINED,
    D1L_FACTORY_RESET_PARTITION_RETAINED_META,
} d1l_factory_reset_partition_t;

typedef enum {
    D1L_FACTORY_RESET_RAW_SLOT_NONE = 0,
    D1L_FACTORY_RESET_RAW_SLOT_PARTITION_START,
    D1L_FACTORY_RESET_RAW_SLOT_PARTITION_END,
} d1l_factory_reset_raw_slot_t;

typedef enum {
    D1L_FACTORY_RESET_DISPOSITION_CLEAR = 0,
    D1L_FACTORY_RESET_DISPOSITION_PRESERVE_PROTOCOL_GUARD,
    D1L_FACTORY_RESET_DISPOSITION_PRESERVE_MIGRATION_EVIDENCE,
    D1L_FACTORY_RESET_DISPOSITION_PRESERVE_TIME_CHECKPOINT,
    D1L_FACTORY_RESET_DISPOSITION_PRESERVE_CRASH_FORENSICS,
    D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE,
    D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL,
} d1l_factory_reset_disposition_t;

typedef enum {
    D1L_FACTORY_RESET_PHASE_IDLE = 0,
    D1L_FACTORY_RESET_PHASE_ACTIVE,
    D1L_FACTORY_RESET_PHASE_COMPLETE,
    D1L_FACTORY_RESET_PHASE_QUARANTINED_NEWER_SCHEMA,
    D1L_FACTORY_RESET_PHASE_QUARANTINED_CORRUPT,
    D1L_FACTORY_RESET_PHASE_STORAGE_ERROR,
} d1l_factory_reset_phase_t;

typedef enum {
    D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES = 0,
    D1L_FACTORY_RESET_SD_STORE_DM_MESSAGES,
    D1L_FACTORY_RESET_SD_STORE_ROUTES,
    D1L_FACTORY_RESET_SD_STORE_PACKET_LOG,
    D1L_FACTORY_RESET_SD_STORE_NODES,
    D1L_FACTORY_RESET_SD_STORE_CONTACTS,
    D1L_FACTORY_RESET_SD_STORE_READ_STATE,
} d1l_factory_reset_sd_store_t;

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t store;
    uint32_t generation;
    uint32_t generation_inverse;
    uint32_t reserved;
    uint32_t checksum;
} d1l_factory_reset_sd_media_marker_t;

typedef struct {
    const char *name;
    d1l_factory_reset_partition_t partition;
    const char *partition_label;
    const char *nvs_namespace;
    const char *key;
    d1l_factory_reset_disposition_t disposition;
    bool contains_secret;
    const char *policy_reason;
    /* NVS entries use RAW_SLOT_NONE. The two d1l_ret_meta entries describe
     * raw ownership-marker ranges that this reset must preserve; they are not
     * NVS namespaces or keys. */
    d1l_factory_reset_raw_slot_t raw_slot;
    uint32_t raw_length;
} d1l_factory_reset_inventory_entry_t;

typedef struct {
    d1l_factory_reset_phase_t phase;
    bool journal_found;
    bool reset_pending;
    bool reset_complete;
    bool completed_journal_cleaned;
    bool failure_telemetry_persisted;
    /* Request-only write classification. A set/commit error is not safe to
     * report as definitely unscheduled when the NVS API says the write may
     * have applied. Runtime callers must keep producers quiesced and restart
     * when request_write_may_have_applied and either reset_pending or
     * request_outcome_ambiguous is true. */
    bool request_commit_attempted;
    bool request_write_may_have_applied;
    bool request_readback_exact;
    bool request_outcome_ambiguous;
    uint32_t sd_lineage_generation;
    uint32_t sd_lineage_active_mask;
    uint32_t attempt_count;
    uint32_t domains_total;
    uint32_t domains_completed;
    uint32_t keys_erased;
    uint32_t keys_already_absent;
    uint32_t next_domain_index;
    uint32_t last_failed_domain_index;
    char last_failed_domain[D1L_FACTORY_RESET_DOMAIN_NAME_LEN];
    esp_err_t last_error;
    /* NVS key deletion is individually committed. It is not a global atomic
     * transaction and it is not a physical flash scrub. */
    bool global_atomic;
    bool physical_flash_scrubbed;
    /* This coordinator never probes, mounts, writes, deletes, or formats SD. */
    bool sd_touched;
    bool removable_sd_data_preserved;
    /* Unknown keys remain untouched because reset uses an exact key inventory,
     * never namespace/partition erase. */
    bool unknown_keys_preserved;
    bool protocol_high_water_preserved;
    bool migration_evidence_preserved;
    bool time_checkpoint_preserved;
    bool crash_forensics_preserved;
    bool retained_ownership_evidence_preserved;
} d1l_factory_reset_status_t;

size_t d1l_factory_reset_inventory_count(void);
bool d1l_factory_reset_inventory_entry(
    size_t index, d1l_factory_reset_inventory_entry_t *out_entry);
const char *d1l_factory_reset_phase_name(d1l_factory_reset_phase_t phase);
const char *d1l_factory_reset_disposition_name(
    d1l_factory_reset_disposition_t disposition);

/* Read-only inspection. A malformed or future journal is quarantined and is
 * never overwritten by inspection, resume, or a new reset request. */
esp_err_t d1l_factory_reset_inspect(d1l_factory_reset_status_t *out_status);

/* Boot-time resume. Call after default and retained NVS initialization and
 * before any producer that can rewrite a retained domain. An active journal is
 * replayed from domain zero; a completed journal is removed. */
esp_err_t d1l_factory_reset_resume(d1l_factory_reset_status_t *out_status);

/* Durably schedule a reset without erasing user domains. Runtime callers keep
 * their reboot quiesces held after this returns successfully and immediately
 * reset the system. The early-boot resume boundary performs the actual key
 * deletion before radio, storage, or connectivity producers can start. A
 * non-OK set/commit result can still have applied: when the returned status
 * reports request_write_may_have_applied plus reset_pending or
 * request_outcome_ambiguous, callers must also keep quiesces and restart. */
esp_err_t d1l_factory_reset_request(d1l_factory_reset_status_t *out_status);

/* A durable per-store lineage fence prevents a removable-SD history created
 * before the last reset from being adopted after reset, including when media
 * was absent during reset. A store may clear only its exact active generation
 * after all of its old primary/alias/segment paths have been durably replaced
 * or erased. Invalid fence state is fail-closed. */
esp_err_t d1l_factory_reset_sd_lineage_snapshot(
    d1l_factory_reset_sd_store_t store, bool *out_active,
    uint32_t *out_generation);
esp_err_t d1l_factory_reset_sd_lineage_clear(
    d1l_factory_reset_sd_store_t store, uint32_t expected_generation);
void d1l_factory_reset_sd_media_marker_init(
    d1l_factory_reset_sd_media_marker_t *marker,
    d1l_factory_reset_sd_store_t store, uint32_t generation);
bool d1l_factory_reset_sd_media_marker_matches(
    const d1l_factory_reset_sd_media_marker_t *marker, size_t marker_length,
    d1l_factory_reset_sd_store_t store, uint32_t generation);

/* Explicit recovery-only replacement for a corrupt, future-schema, or
 * unreadable journal. Call only after the recovery console's two independent
 * confirmations. It never erases the quarantined journal implicitly. */
esp_err_t d1l_factory_reset_repair_quarantined(
    d1l_factory_reset_status_t *out_status);
