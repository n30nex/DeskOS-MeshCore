#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "storage/factory_reset.h"

#define MOCK_ENTRY_COUNT 64U
#define MOCK_HANDLE_COUNT 8U
#define MOCK_DATA_MAX 256U
#define MOCK_PARTITION_LEN 20U
#define MOCK_NAMESPACE_LEN 24U
#define MOCK_KEY_LEN 24U

typedef struct {
    bool used;
    char partition[MOCK_PARTITION_LEN];
    char nvs_namespace[MOCK_NAMESPACE_LEN];
    char key[MOCK_KEY_LEN];
    uint8_t data[MOCK_DATA_MAX];
    size_t length;
} mock_entry_t;

typedef enum {
    MOCK_PENDING_NONE = 0,
    MOCK_PENDING_SET,
    MOCK_PENDING_ERASE,
} mock_pending_t;

typedef struct {
    bool used;
    char partition[MOCK_PARTITION_LEN];
    char nvs_namespace[MOCK_NAMESPACE_LEN];
    mock_pending_t pending;
    char pending_key[MOCK_KEY_LEN];
    uint8_t pending_data[MOCK_DATA_MAX];
    size_t pending_length;
} mock_handle_t;

static mock_entry_t s_entries[MOCK_ENTRY_COUNT];
static mock_handle_t s_handles[MOCK_HANDLE_COUNT];
static size_t s_commit_calls;
static size_t s_set_calls;
static size_t s_erase_calls;
static size_t s_open_calls;
static size_t s_fail_commit_call;
static size_t s_fail_commit_after_apply_call;
static size_t s_fail_set_after_apply_call;
static size_t s_fail_erase_call;
static size_t s_fail_open_call;

static void secure_zero(void *data, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;
    for (size_t i = 0U; i < length; ++i) {
        bytes[i] = 0U;
    }
}

static void mock_reset(void)
{
    secure_zero(s_entries, sizeof(s_entries));
    secure_zero(s_handles, sizeof(s_handles));
    s_commit_calls = 0U;
    s_set_calls = 0U;
    s_erase_calls = 0U;
    s_open_calls = 0U;
    s_fail_commit_call = 0U;
    s_fail_commit_after_apply_call = 0U;
    s_fail_set_after_apply_call = 0U;
    s_fail_erase_call = 0U;
    s_fail_open_call = 0U;
}

static mock_entry_t *find_entry(const char *partition,
                                const char *nvs_namespace,
                                const char *key)
{
    for (size_t i = 0U; i < MOCK_ENTRY_COUNT; ++i) {
        if (s_entries[i].used &&
            strcmp(s_entries[i].partition, partition) == 0 &&
            strcmp(s_entries[i].nvs_namespace, nvs_namespace) == 0 &&
            strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static bool namespace_exists(const char *partition,
                             const char *nvs_namespace)
{
    for (size_t i = 0U; i < MOCK_ENTRY_COUNT; ++i) {
        if (s_entries[i].used &&
            strcmp(s_entries[i].partition, partition) == 0 &&
            strcmp(s_entries[i].nvs_namespace, nvs_namespace) == 0) {
            return true;
        }
    }
    return false;
}

static mock_entry_t *allocate_entry(const char *partition,
                                    const char *nvs_namespace,
                                    const char *key)
{
    mock_entry_t *entry = find_entry(partition, nvs_namespace, key);
    if (entry) {
        return entry;
    }
    for (size_t i = 0U; i < MOCK_ENTRY_COUNT; ++i) {
        if (!s_entries[i].used) {
            s_entries[i].used = true;
            (void)snprintf(s_entries[i].partition,
                           sizeof(s_entries[i].partition), "%s", partition);
            (void)snprintf(s_entries[i].nvs_namespace,
                           sizeof(s_entries[i].nvs_namespace), "%s",
                           nvs_namespace);
            (void)snprintf(s_entries[i].key, sizeof(s_entries[i].key), "%s",
                           key);
            return &s_entries[i];
        }
    }
    return NULL;
}

static bool mock_seed(const char *partition, const char *nvs_namespace,
                      const char *key, const void *data, size_t length)
{
    if (!partition || !nvs_namespace || !key || !data ||
        length > MOCK_DATA_MAX) {
        return false;
    }
    mock_entry_t *entry = allocate_entry(partition, nvs_namespace, key);
    if (!entry) {
        return false;
    }
    secure_zero(entry->data, sizeof(entry->data));
    memcpy(entry->data, data, length);
    entry->length = length;
    return true;
}

static size_t mock_copy(const char *partition, const char *nvs_namespace,
                        const char *key, void *out, size_t out_size)
{
    mock_entry_t *entry = find_entry(partition, nvs_namespace, key);
    if (!entry) {
        return 0U;
    }
    if (out && out_size >= entry->length) {
        memcpy(out, entry->data, entry->length);
    }
    return entry->length;
}

static mock_handle_t *handle_for(nvs_handle_t handle)
{
    if (handle == 0U || handle > MOCK_HANDLE_COUNT) {
        return NULL;
    }
    return s_handles[handle - 1U].used ? &s_handles[handle - 1U] : NULL;
}

static esp_err_t mock_open(const char *partition, const char *nvs_namespace,
                           nvs_open_mode_t mode, nvs_handle_t *out_handle)
{
    if (!partition || !nvs_namespace || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    s_open_calls++;
    if (s_fail_open_call != 0U && s_open_calls == s_fail_open_call) {
        s_fail_open_call = 0U;
        return ESP_FAIL;
    }
    if (mode == NVS_READONLY && !namespace_exists(partition, nvs_namespace)) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    for (size_t i = 0U; i < MOCK_HANDLE_COUNT; ++i) {
        if (!s_handles[i].used) {
            s_handles[i].used = true;
            (void)snprintf(s_handles[i].partition,
                           sizeof(s_handles[i].partition), "%s", partition);
            (void)snprintf(s_handles[i].nvs_namespace,
                           sizeof(s_handles[i].nvs_namespace), "%s",
                           nvs_namespace);
            *out_handle = (nvs_handle_t)(i + 1U);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    return mock_open("nvs", namespace_name, open_mode, out_handle);
}

esp_err_t nvs_open_from_partition(const char *part_name,
                                  const char *namespace_name,
                                  nvs_open_mode_t open_mode,
                                  nvs_handle_t *out_handle)
{
    return mock_open(part_name, namespace_name, open_mode, out_handle);
}

void nvs_close(nvs_handle_t handle)
{
    mock_handle_t *slot = handle_for(handle);
    if (slot) {
        secure_zero(slot, sizeof(*slot));
    }
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length)
{
    mock_handle_t *slot = handle_for(handle);
    if (!slot || !key || !length) {
        return ESP_ERR_INVALID_ARG;
    }
    mock_entry_t *entry = find_entry(slot->partition, slot->nvs_namespace, key);
    if (!entry) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    const size_t available = *length;
    *length = entry->length;
    if (!out_value) {
        return ESP_OK;
    }
    if (available < entry->length) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, entry->data, entry->length);
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    mock_handle_t *slot = handle_for(handle);
    if (!slot || !key || !value || length > sizeof(slot->pending_data)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_set_calls++;
    if (s_fail_set_after_apply_call != 0U &&
        s_set_calls == s_fail_set_after_apply_call) {
        s_fail_set_after_apply_call = 0U;
        mock_entry_t *entry = allocate_entry(
            slot->partition, slot->nvs_namespace, key);
        if (!entry) {
            return ESP_ERR_NO_MEM;
        }
        secure_zero(entry->data, sizeof(entry->data));
        memcpy(entry->data, value, length);
        entry->length = length;
        return ESP_ERR_NVS_REMOVE_FAILED;
    }
    (void)snprintf(slot->pending_key, sizeof(slot->pending_key), "%s", key);
    memcpy(slot->pending_data, value, length);
    slot->pending_length = length;
    slot->pending = MOCK_PENDING_SET;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    mock_handle_t *slot = handle_for(handle);
    if (!slot || !key) {
        return ESP_ERR_INVALID_ARG;
    }
    s_erase_calls++;
    if (s_fail_erase_call != 0U && s_erase_calls == s_fail_erase_call) {
        s_fail_erase_call = 0U;
        return ESP_FAIL;
    }
    if (!find_entry(slot->partition, slot->nvs_namespace, key)) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    (void)snprintf(slot->pending_key, sizeof(slot->pending_key), "%s", key);
    slot->pending = MOCK_PENDING_ERASE;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    mock_handle_t *slot = handle_for(handle);
    if (!slot) {
        return ESP_ERR_INVALID_ARG;
    }
    s_commit_calls++;
    if (s_fail_commit_call != 0U && s_commit_calls == s_fail_commit_call) {
        s_fail_commit_call = 0U;
        return ESP_FAIL;
    }
    const bool fail_after_apply = s_fail_commit_after_apply_call != 0U &&
        s_commit_calls == s_fail_commit_after_apply_call;
    if (fail_after_apply) {
        s_fail_commit_after_apply_call = 0U;
    }
    if (slot->pending == MOCK_PENDING_SET) {
        mock_entry_t *entry = allocate_entry(
            slot->partition, slot->nvs_namespace, slot->pending_key);
        if (!entry) {
            return ESP_ERR_NO_MEM;
        }
        secure_zero(entry->data, sizeof(entry->data));
        memcpy(entry->data, slot->pending_data, slot->pending_length);
        entry->length = slot->pending_length;
    } else if (slot->pending == MOCK_PENDING_ERASE) {
        mock_entry_t *entry = find_entry(
            slot->partition, slot->nvs_namespace, slot->pending_key);
        if (entry) {
            secure_zero(entry, sizeof(*entry));
        }
    }
    slot->pending = MOCK_PENDING_NONE;
    secure_zero(slot->pending_data, sizeof(slot->pending_data));
    slot->pending_length = 0U;
    slot->pending_key[0] = '\0';
    return fail_after_apply ? ESP_ERR_NVS_REMOVE_FAILED : ESP_OK;
}

static void seed_inventory(bool include_preserved)
{
    static const uint8_t secret[] =
        "wifi-password identity-private-key channel-secret";
    static const uint8_t ordinary[] = "retained-user-state";
    static const uint8_t preserved[] = "preserved-forensic-invariant";
    for (size_t i = 0U; i < d1l_factory_reset_inventory_count(); ++i) {
        d1l_factory_reset_inventory_entry_t entry = {0};
        assert(d1l_factory_reset_inventory_entry(i, &entry));
        if (entry.disposition == D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL) {
            continue;
        }
        if (entry.raw_slot != D1L_FACTORY_RESET_RAW_SLOT_NONE) {
            assert(entry.partition ==
                   D1L_FACTORY_RESET_PARTITION_RETAINED_META);
            assert(strcmp(entry.partition_label, "d1l_ret_meta") == 0);
            assert(entry.nvs_namespace[0] == '\0');
            assert(entry.key[0] == '\0');
            assert(entry.raw_length == D1L_FACTORY_RESET_RAW_MARKER_BYTES);
            continue;
        }
        if (entry.disposition == D1L_FACTORY_RESET_DISPOSITION_CLEAR) {
            const void *data = entry.contains_secret ? (const void *)secret :
                                                        (const void *)ordinary;
            const size_t length = entry.contains_secret ? sizeof(secret) :
                                                          sizeof(ordinary);
            assert(mock_seed(entry.partition_label, entry.nvs_namespace,
                             entry.key, data, length));
        } else if (include_preserved) {
            assert(mock_seed(entry.partition_label, entry.nvs_namespace,
                             entry.key, preserved, sizeof(preserved)));
        }
    }
}

static void assert_clear_entries_absent(void)
{
    for (size_t i = 0U; i < D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT; ++i) {
        d1l_factory_reset_inventory_entry_t entry = {0};
        assert(d1l_factory_reset_inventory_entry(i, &entry));
        assert(entry.disposition == D1L_FACTORY_RESET_DISPOSITION_CLEAR);
        assert(mock_copy(entry.partition_label, entry.nvs_namespace,
                         entry.key, NULL, 0U) == 0U);
    }
}

static void assert_preserved_entries_unchanged(void)
{
    static const uint8_t preserved[] = "preserved-forensic-invariant";
    uint8_t actual[sizeof(preserved)] = {0};
    for (size_t i = D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT;
         i < d1l_factory_reset_inventory_count(); ++i) {
        d1l_factory_reset_inventory_entry_t entry = {0};
        assert(d1l_factory_reset_inventory_entry(i, &entry));
        if (entry.disposition == D1L_FACTORY_RESET_DISPOSITION_INTERNAL_JOURNAL) {
            continue;
        }
        if (entry.raw_slot != D1L_FACTORY_RESET_RAW_SLOT_NONE) {
            continue;
        }
        memset(actual, 0, sizeof(actual));
        assert(mock_copy(entry.partition_label, entry.nvs_namespace,
                         entry.key, actual, sizeof(actual)) ==
               sizeof(preserved));
        assert(memcmp(actual, preserved, sizeof(preserved)) == 0);
    }
}

static uint32_t assert_all_sd_lineages(bool expected_active)
{
    uint32_t common_generation = 0U;
    for (uint32_t i = 0U; i < D1L_FACTORY_RESET_SD_STORE_COUNT; ++i) {
        bool active = !expected_active;
        uint32_t generation = 0U;
        assert(d1l_factory_reset_sd_lineage_snapshot(
                   (d1l_factory_reset_sd_store_t)i, &active,
                   &generation) == ESP_OK);
        assert(active == expected_active);
        assert(generation != 0U);
        if (common_generation == 0U) {
            common_generation = generation;
        }
        assert(generation == common_generation);
    }
    return common_generation;
}

static void test_inventory_and_complete_reset(void)
{
    mock_reset();
    assert(d1l_factory_reset_inventory_count() ==
           D1L_FACTORY_RESET_INVENTORY_COUNT);
    size_t raw_marker_count = 0U;
    for (size_t i = 0U; i < d1l_factory_reset_inventory_count(); ++i) {
        d1l_factory_reset_inventory_entry_t entry = {0};
        assert(d1l_factory_reset_inventory_entry(i, &entry));
        if (entry.raw_slot != D1L_FACTORY_RESET_RAW_SLOT_NONE) {
            raw_marker_count++;
            assert(entry.disposition ==
                   D1L_FACTORY_RESET_DISPOSITION_PRESERVE_OWNERSHIP_EVIDENCE);
            assert(entry.partition ==
                   D1L_FACTORY_RESET_PARTITION_RETAINED_META);
            assert(strcmp(entry.partition_label, "d1l_ret_meta") == 0);
            assert(entry.raw_length == D1L_FACTORY_RESET_RAW_MARKER_BYTES);
        }
    }
    assert(raw_marker_count == D1L_FACTORY_RESET_RAW_MARKER_COUNT);
    seed_inventory(true);

    d1l_factory_reset_status_t status = {0};
    assert(d1l_factory_reset_request(&status) == ESP_OK);
    assert(status.phase == D1L_FACTORY_RESET_PHASE_ACTIVE);
    assert(status.reset_pending);
    assert(!status.reset_complete);
    assert(status.attempt_count == 0U);
    assert(status.domains_completed == 0U);
    assert(s_erase_calls == 0U);
    assert(d1l_factory_reset_resume(&status) == ESP_OK);
    assert(status.phase == D1L_FACTORY_RESET_PHASE_COMPLETE);
    assert(status.reset_complete);
    assert(!status.reset_pending);
    assert(status.attempt_count == 1U);
    assert(status.domains_total == D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT);
    assert(status.domains_completed == D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT);
    assert(status.keys_erased == D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT);
    assert(status.keys_already_absent == 0U);
    assert(status.sd_lineage_generation == 1U);
    assert(status.sd_lineage_active_mask == 0x1fU);
    assert(!status.global_atomic);
    assert(!status.physical_flash_scrubbed);
    assert(!status.sd_touched);
    assert(status.removable_sd_data_preserved);
    assert(status.unknown_keys_preserved);
    assert(status.protocol_high_water_preserved);
    assert(status.migration_evidence_preserved);
    assert(status.time_checkpoint_preserved);
    assert(status.crash_forensics_preserved);
    assert(status.retained_ownership_evidence_preserved);
    assert_clear_entries_absent();
    assert_preserved_entries_unchanged();
    const uint32_t reset_generation = assert_all_sd_lineages(true);
    assert(mock_copy("nvs", "d1l_reset", "factory_v1", NULL, 0U) > 0U);

    memset(&status, 0, sizeof(status));
    assert(d1l_factory_reset_resume(&status) == ESP_OK);
    assert(status.reset_complete);
    assert(status.completed_journal_cleaned);
    assert(!status.journal_found);
    assert(mock_copy("nvs", "d1l_reset", "factory_v1", NULL, 0U) == 0U);
    assert_clear_entries_absent();
    assert_preserved_entries_unchanged();
    assert_all_sd_lineages(true);

    memset(&status, 0, sizeof(status));
    assert(d1l_factory_reset_resume(&status) == ESP_OK);
    assert(status.phase == D1L_FACTORY_RESET_PHASE_IDLE);

    /* Reset while removable media is absent leaves a durable fence after the
     * journal is cleaned. A later insertion cannot be adopted until each
     * exact store lineage has been replaced or erased successfully. */
    for (uint32_t i = 0U; i < D1L_FACTORY_RESET_SD_STORE_COUNT; ++i) {
        assert(d1l_factory_reset_sd_lineage_clear(
                   (d1l_factory_reset_sd_store_t)i,
                   reset_generation + 1U) == ESP_ERR_INVALID_STATE);
        assert(d1l_factory_reset_sd_lineage_clear(
                   (d1l_factory_reset_sd_store_t)i,
                   reset_generation) == ESP_OK);
    }
    assert_all_sd_lineages(false);
}

static void test_request_commit_fail_after_apply_is_read_back_and_restarted(void)
{
    mock_reset();
    seed_inventory(false);
    s_fail_commit_after_apply_call = 1U;

    d1l_factory_reset_status_t status = {0};
    assert(d1l_factory_reset_request(&status) ==
           ESP_ERR_NVS_REMOVE_FAILED);
    assert(status.request_commit_attempted);
    assert(status.request_write_may_have_applied);
    assert(status.request_readback_exact);
    assert(!status.request_outcome_ambiguous);
    assert(status.phase == D1L_FACTORY_RESET_PHASE_ACTIVE);
    assert(status.journal_found);
    assert(status.reset_pending);
    assert(!status.reset_complete);
    assert(status.domains_completed == 0U);
    assert(s_erase_calls == 0U);
    assert(mock_copy("nvs", "d1l_reset", "factory_v1", NULL, 0U) > 0U);

    /* This models the mandatory controlled restart after the readback. */
    assert(d1l_factory_reset_resume(&status) == ESP_OK);
    assert_clear_entries_absent();

    mock_reset();
    seed_inventory(false);
    /* Journal inspection plus four lineage-generation reads precede the
     * writer open. */
    s_fail_open_call = 6U;
    memset(&status, 0, sizeof(status));
    assert(d1l_factory_reset_request(&status) == ESP_FAIL);
    assert(!status.request_commit_attempted);
    assert(!status.request_write_may_have_applied);
    assert(!status.request_readback_exact);
    assert(!status.request_outcome_ambiguous);
    assert(!status.reset_pending);
    assert(s_erase_calls == 0U);
    assert(mock_copy("nvs", "d1l_reset", "factory_v1", NULL, 0U) == 0U);
}

static void test_sd_media_marker_card_swap_and_clear_power_cuts(void)
{
    mock_reset();
    seed_inventory(false);
    d1l_factory_reset_status_t status = {0};
    assert(d1l_factory_reset_request(&status) == ESP_OK);
    assert(d1l_factory_reset_resume(&status) == ESP_OK);
    const uint32_t generation = status.sd_lineage_generation;
    assert(generation != 0U);

    d1l_factory_reset_sd_media_marker_t card_a = {0};
    d1l_factory_reset_sd_media_marker_t card_b = {0};
    d1l_factory_reset_sd_media_marker_init(
        &card_a, D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES, generation);
    d1l_factory_reset_sd_media_marker_init(
        &card_b, D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES,
        generation + 1U);
    assert(d1l_factory_reset_sd_media_marker_matches(
        &card_a, sizeof(card_a),
        D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES, generation));
    assert(!d1l_factory_reset_sd_media_marker_matches(
        &card_b, sizeof(card_b),
        D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES, generation));

    /* A cut before the NVS clear applies leaves the onboard fence active even
     * though card A's marker is already durable. */
    s_fail_commit_call = s_commit_calls + 1U;
    assert(d1l_factory_reset_sd_lineage_clear(
               D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES,
               generation) == ESP_FAIL);
    bool active = false;
    uint32_t readback_generation = 0U;
    assert(d1l_factory_reset_sd_lineage_snapshot(
               D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES, &active,
               &readback_generation) == ESP_OK);
    assert(active);
    assert(readback_generation == generation);
    const bool ready_after_failed_clear = !active &&
        d1l_factory_reset_sd_media_marker_matches(
            &card_a, sizeof(card_a),
            D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES, generation);
    assert(!ready_after_failed_clear);

    /* A fail-after-apply clear is safe because the exact card marker predates
     * the commit. Card A is accepted after reboot; swapped card B is not. */
    s_fail_commit_after_apply_call = s_commit_calls + 1U;
    assert(d1l_factory_reset_sd_lineage_clear(
               D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES,
               generation) == ESP_ERR_NVS_REMOVE_FAILED);
    assert(d1l_factory_reset_sd_lineage_snapshot(
               D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES, &active,
               &readback_generation) == ESP_OK);
    assert(!active);
    const bool card_a_ready = !active &&
        d1l_factory_reset_sd_media_marker_matches(
            &card_a, sizeof(card_a),
            D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES, generation);
    const bool card_b_ready = !active &&
        d1l_factory_reset_sd_media_marker_matches(
            &card_b, sizeof(card_b),
            D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES, generation);
    assert(card_a_ready);
    assert(!card_b_ready);
}

static void test_request_set_fail_after_apply_is_read_back_and_restarted(void)
{
    mock_reset();
    seed_inventory(false);
    s_fail_set_after_apply_call = 1U;

    d1l_factory_reset_status_t status = {0};
    assert(d1l_factory_reset_request(&status) ==
           ESP_ERR_NVS_REMOVE_FAILED);
    assert(!status.request_commit_attempted);
    assert(status.request_write_may_have_applied);
    assert(status.request_readback_exact);
    assert(!status.request_outcome_ambiguous);
    assert(status.phase == D1L_FACTORY_RESET_PHASE_ACTIVE);
    assert(status.journal_found);
    assert(status.reset_pending);
    assert(!status.reset_complete);
    assert(status.domains_completed == 0U);
    assert(s_commit_calls == 0U);
    assert(s_erase_calls == 0U);
    assert(mock_copy("nvs", "d1l_reset", "factory_v1", NULL, 0U) > 0U);

    /* This models the mandatory controlled restart after the readback. */
    assert(d1l_factory_reset_resume(&status) == ESP_OK);
    assert_clear_entries_absent();
}

static void test_partial_failure_replays_from_zero(void)
{
    mock_reset();
    seed_inventory(false);
    s_fail_erase_call = 5U;

    d1l_factory_reset_status_t status = {0};
    assert(d1l_factory_reset_request(&status) == ESP_OK);
    assert(s_erase_calls == 0U);
    assert(d1l_factory_reset_resume(&status) == ESP_FAIL);
    assert(status.phase == D1L_FACTORY_RESET_PHASE_ACTIVE);
    assert(status.reset_pending);
    assert(status.domains_completed == 4U);
    assert(status.last_failed_domain_index == 4U);
    assert(strcmp(status.last_failed_domain, "heard_nodes") == 0);
    assert(status.last_error == ESP_FAIL);
    assert(status.failure_telemetry_persisted);

    /* Inspection transport succeeds, but the durable reset failure remains
     * visible with its exact phase, domain, and progress. Console callers
     * must not replace this stored error with the ESP_OK inspection result. */
    d1l_factory_reset_status_t inspected = {0};
    assert(d1l_factory_reset_inspect(&inspected) == ESP_OK);
    assert(inspected.phase == D1L_FACTORY_RESET_PHASE_ACTIVE);
    assert(inspected.reset_pending);
    assert(!inspected.reset_complete);
    assert(inspected.attempt_count == 1U);
    assert(inspected.domains_completed == 4U);
    assert(inspected.keys_erased == 4U);
    assert(inspected.keys_already_absent == 0U);
    assert(inspected.next_domain_index == 4U);
    assert(inspected.last_failed_domain_index == 4U);
    assert(strcmp(inspected.last_failed_domain, "heard_nodes") == 0);
    assert(inspected.last_error == ESP_FAIL);

    d1l_factory_reset_inventory_entry_t first = {0};
    assert(d1l_factory_reset_inventory_entry(0U, &first));
    static const uint8_t recreated_secret[] = "recreated-secret";
    assert(mock_seed(first.partition_label, first.nvs_namespace, first.key,
                     recreated_secret, sizeof(recreated_secret)));

    memset(&status, 0, sizeof(status));
    assert(d1l_factory_reset_resume(&status) == ESP_OK);
    assert(status.attempt_count == 2U);
    assert(status.domains_completed == D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT);
    assert_clear_entries_absent();
}

static void test_interruption_windows_are_idempotent(void)
{
    /* Request commit is call 1; retry checkpoint is call 2, so commit 3 is the
     * first durable SD lineage fence. A cut here must prevent every key erase. */
    mock_reset();
    seed_inventory(false);
    d1l_factory_reset_status_t status = {0};
    assert(d1l_factory_reset_request(&status) == ESP_OK);
    s_fail_commit_call = 3U;
    assert(d1l_factory_reset_resume(&status) == ESP_FAIL);
    assert(s_erase_calls == 0U);
    d1l_factory_reset_inventory_entry_t first = {0};
    assert(d1l_factory_reset_inventory_entry(0U, &first));
    assert(mock_copy(first.partition_label, first.nvs_namespace, first.key,
                     NULL, 0U) > 0U);
    assert(d1l_factory_reset_resume(&status) == ESP_OK);
    assert_clear_entries_absent();

    /* Four lineage commits shift the first erase/progress commits to 7/8.
     * A cut at progress commit 8 leaves
     * the intent behind the data; replay must accept NOT_FOUND and continue. */
    mock_reset();
    seed_inventory(false);
    memset(&status, 0, sizeof(status));
    assert(d1l_factory_reset_request(&status) == ESP_OK);
    s_fail_commit_call = 8U;
    assert(d1l_factory_reset_resume(&status) == ESP_FAIL);
    assert(mock_copy(first.partition_label, first.nvs_namespace, first.key,
                     NULL, 0U) == 0U);
    assert(d1l_factory_reset_resume(&status) == ESP_OK);
    assert(status.keys_already_absent >= 1U);
    assert_clear_entries_absent();
}

static void test_corrupt_and_newer_journals_fail_closed(void)
{
    mock_reset();
    seed_inventory(false);
    uint8_t corrupt[48] = {0};
    assert(mock_seed("nvs", "d1l_reset", "factory_v1", corrupt,
                     sizeof(corrupt)));
    d1l_factory_reset_status_t status = {0};
    const size_t erase_before = s_erase_calls;
    assert(d1l_factory_reset_resume(&status) == ESP_ERR_INVALID_STATE);
    assert(status.phase == D1L_FACTORY_RESET_PHASE_QUARANTINED_CORRUPT);
    assert(s_erase_calls == erase_before);
    assert(mock_copy("nvs", "d1l_reset", "factory_v1", NULL, 0U) ==
           sizeof(corrupt));
    assert(d1l_factory_reset_repair_quarantined(&status) == ESP_OK);
    assert(status.phase == D1L_FACTORY_RESET_PHASE_ACTIVE);
    assert(status.request_readback_exact);
    assert(status.next_domain_index == 0U);
    assert(status.sd_lineage_generation == 1U);
    assert(d1l_factory_reset_resume(&status) == ESP_OK);
    assert_all_sd_lineages(true);

    mock_reset();
    seed_inventory(false);
    uint8_t future[52] = {0};
    assert(mock_seed("nvs", "d1l_reset", "factory_v1", future,
                     sizeof(future)));
    assert(d1l_factory_reset_request(&status) == ESP_ERR_NOT_SUPPORTED);
    assert(status.phase ==
           D1L_FACTORY_RESET_PHASE_QUARANTINED_NEWER_SCHEMA);
    assert(s_erase_calls == 0U);
    assert(mock_copy("nvs", "d1l_reset", "factory_v1", NULL, 0U) ==
           sizeof(future));
    assert(d1l_factory_reset_repair_quarantined(&status) == ESP_OK);
    assert(status.phase == D1L_FACTORY_RESET_PHASE_ACTIVE);
    assert(status.next_domain_index == 0U);
}

int main(void)
{
    test_inventory_and_complete_reset();
    test_request_commit_fail_after_apply_is_read_back_and_restarted();
    test_sd_media_marker_card_swap_and_clear_power_cuts();
    test_request_set_fail_after_apply_is_read_back_and_restarted();
    test_partial_failure_replays_from_zero();
    test_interruption_windows_are_idempotent();
    test_corrupt_and_newer_journals_fail_closed();
    puts("native factory reset coordinator: ok");
    return 0;
}
