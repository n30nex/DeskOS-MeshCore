#include "retained_blob_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/release_profile.h"
#include "freertos/FreeRTOS.h"
#include "esp_partition.h"
#include "hal/rp2040_bridge.h"
#include "mesh/route_store_worker.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "storage/factory_reset.h"

#define D1L_RETAINED_NVS_PARTITION "d1l_retained"
#define D1L_RETAINED_NVS_META_PARTITION "d1l_ret_meta"
#define D1L_RETAINED_NVS_META_MAGIC 0x44314C52U
#define D1L_RETAINED_NVS_META_LEGACY_VERSION 1U
#define D1L_RETAINED_NVS_META_VERSION 2U
#define D1L_RETAINED_NVS_SENTINEL_NAMESPACE "d1l_ret_meta"
#define D1L_RETAINED_NVS_SENTINEL_KEY "initialized"
#define D1L_RETAINED_NVS_ANCHOR_KEY "anchor"
#define D1L_RETAINED_PUBLIC_MESSAGE_NAMESPACE "d1l_messages"
#define D1L_RETAINED_DM_MESSAGE_NAMESPACE "d1l_dms"
#define D1L_RETAINED_ROUTE_NAMESPACE "d1l_routes"
#define D1L_RETAINED_PACKET_LOG_NAMESPACE "d1l_packets"
#define D1L_RETAINED_PUBLIC_MESSAGE_SD_DIR "stores/messages/public"
#define D1L_RETAINED_DM_MESSAGE_SD_DIR "stores/messages/dm"
#define D1L_RETAINED_ROUTE_SD_DIR "stores/routes"
#define D1L_RETAINED_PACKET_LOG_SD_DIR "stores/packet_log"
#define D1L_RETAINED_SD_LINEAGE_MARKER_KEY "reset_lineage_v1"
#define D1L_RETAINED_SD_WRITE_TIMEOUT_MS 750U
#define D1L_RETAINED_SD_READ_TIMEOUT_MS 750U
#define D1L_RETAINED_NVS_MIGRATION_MAX_BYTES 8192U

typedef struct {
    d1l_retained_blob_store_id_t id;
    const char *name;
    const char *nvs_namespace;
    const char *sd_directory;
} d1l_retained_blob_store_config_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t magic_inverse;
    uint32_t version_inverse;
} d1l_retained_nvs_marker_t;

_Static_assert(sizeof(d1l_retained_nvs_marker_t) == 16U,
               "retained ownership marker layout must remain stable");

typedef enum {
    D1L_RETAINED_SD_OP_READ,
    D1L_RETAINED_SD_OP_WRITE,
    D1L_RETAINED_SD_OP_RENAME,
} d1l_retained_blob_store_sd_op_t;

static const d1l_retained_blob_store_config_t s_store_configs[] = {
    {
        .id = D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES,
        .name = "public_messages",
        .nvs_namespace = D1L_RETAINED_PUBLIC_MESSAGE_NAMESPACE,
        .sd_directory = D1L_RETAINED_PUBLIC_MESSAGE_SD_DIR,
    },
    {
        .id = D1L_RETAINED_BLOB_STORE_DM_MESSAGES,
        .name = "dm_messages",
        .nvs_namespace = D1L_RETAINED_DM_MESSAGE_NAMESPACE,
        .sd_directory = D1L_RETAINED_DM_MESSAGE_SD_DIR,
    },
    {
        .id = D1L_RETAINED_BLOB_STORE_ROUTES,
        .name = "routes",
        .nvs_namespace = D1L_RETAINED_ROUTE_NAMESPACE,
        .sd_directory = D1L_RETAINED_ROUTE_SD_DIR,
    },
    {
        .id = D1L_RETAINED_BLOB_STORE_PACKET_LOG,
        .name = "packet_log",
        .nvs_namespace = D1L_RETAINED_PACKET_LOG_NAMESPACE,
        .sd_directory = D1L_RETAINED_PACKET_LOG_SD_DIR,
    },
};

_Static_assert((uint32_t)D1L_RETAINED_BLOB_STORE_COUNT ==
                   (uint32_t)D1L_FACTORY_RESET_SD_STORE_COUNT,
               "retained and reset SD store inventories must match");
_Static_assert((uint32_t)D1L_RETAINED_BLOB_STORE_PACKET_LOG ==
                   (uint32_t)D1L_FACTORY_RESET_SD_STORE_PACKET_LOG,
               "retained and reset SD store identifiers must match");

static bool s_store_sd_enabled[D1L_RETAINED_BLOB_STORE_COUNT];
static uint32_t s_store_backend_generation[D1L_RETAINED_BLOB_STORE_COUNT];
static d1l_retained_blob_store_sd_stats_t s_store_sd_stats[D1L_RETAINED_BLOB_STORE_COUNT];
static portMUX_TYPE s_store_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_retained_nvs_ready;
static esp_err_t s_retained_nvs_error = ESP_ERR_INVALID_STATE;
static bool s_retained_nvs_marker_ready;
static bool s_retained_nvs_markers_complete;
static bool s_retained_nvs_anchor_ready;
static bool s_retained_nvs_sentinel_ready;
static bool s_retained_nvs_external_init_required;
static bool s_retained_nvs_marker_finalize_pending;
static bool s_retained_nvs_initialized_this_boot;
static uint32_t s_retained_nvs_migrated_keys;
static esp_err_t s_retained_nvs_migration_error = ESP_OK;
static d1l_retained_blob_store_nvs_telemetry_t s_retained_nvs_telemetry;

static esp_err_t retained_nvs_unavailable_error(void);
static esp_err_t sd_write_blob_with_lineage(
    const d1l_retained_blob_store_config_t *config, const char *key,
    const void *src, size_t len, uint32_t expected_generation);
static esp_err_t sd_erase_blob_with_lineage(
    const d1l_retained_blob_store_config_t *config, const char *key,
    uint32_t expected_generation);
static esp_err_t sd_media_lineage_ready(
    const d1l_retained_blob_store_config_t *config,
    uint32_t expected_backend_generation, bool *out_ready,
    bool *out_onboard_active, uint32_t *out_reset_generation,
    bool *out_marker_matches);

static void telemetry_add_u64(uint64_t *value, uint64_t increment)
{
    if (!value) {
        return;
    }
    *value = UINT64_MAX - *value < increment ? UINT64_MAX :
        *value + increment;
}

static void note_nvs_write_attempt(
    const d1l_retained_blob_store_config_t *config, size_t len)
{
    if (!config || config->id >= D1L_RETAINED_BLOB_STORE_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_store_state_mux);
    d1l_retained_blob_store_nvs_store_telemetry_t *store =
        &s_retained_nvs_telemetry.stores[config->id];
    telemetry_add_u64(&s_retained_nvs_telemetry.write_attempt_count, 1U);
    telemetry_add_u64(&s_retained_nvs_telemetry.write_bytes_attempted,
                      (uint64_t)len);
    telemetry_add_u64(&store->write_attempt_count, 1U);
    telemetry_add_u64(&store->write_bytes_attempted, (uint64_t)len);
    portEXIT_CRITICAL(&s_store_state_mux);
}

static void note_nvs_write_result(
    const d1l_retained_blob_store_config_t *config, size_t len, esp_err_t ret)
{
    if (!config || config->id >= D1L_RETAINED_BLOB_STORE_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_store_state_mux);
    d1l_retained_blob_store_nvs_store_telemetry_t *store =
        &s_retained_nvs_telemetry.stores[config->id];
    if (ret == ESP_OK) {
        telemetry_add_u64(&s_retained_nvs_telemetry.write_commit_count, 1U);
        telemetry_add_u64(&s_retained_nvs_telemetry.write_bytes_committed,
                          (uint64_t)len);
        telemetry_add_u64(&store->write_commit_count, 1U);
        telemetry_add_u64(&store->write_bytes_committed, (uint64_t)len);
    } else {
        telemetry_add_u64(&s_retained_nvs_telemetry.write_fail_count, 1U);
        telemetry_add_u64(&store->write_fail_count, 1U);
        s_retained_nvs_telemetry.last_error = ret;
        store->last_error = ret;
    }
    portEXIT_CRITICAL(&s_store_state_mux);
}

static void note_nvs_erase_attempt(
    const d1l_retained_blob_store_config_t *config)
{
    if (!config || config->id >= D1L_RETAINED_BLOB_STORE_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_store_state_mux);
    d1l_retained_blob_store_nvs_store_telemetry_t *store =
        &s_retained_nvs_telemetry.stores[config->id];
    telemetry_add_u64(&s_retained_nvs_telemetry.erase_attempt_count, 1U);
    telemetry_add_u64(&store->erase_attempt_count, 1U);
    portEXIT_CRITICAL(&s_store_state_mux);
}

static void note_nvs_erase_result(
    const d1l_retained_blob_store_config_t *config, esp_err_t ret)
{
    if (!config || config->id >= D1L_RETAINED_BLOB_STORE_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_store_state_mux);
    d1l_retained_blob_store_nvs_store_telemetry_t *store =
        &s_retained_nvs_telemetry.stores[config->id];
    if (ret == ESP_OK) {
        telemetry_add_u64(&s_retained_nvs_telemetry.erase_commit_count, 1U);
        telemetry_add_u64(&store->erase_commit_count, 1U);
    } else {
        telemetry_add_u64(&s_retained_nvs_telemetry.erase_fail_count, 1U);
        telemetry_add_u64(&store->erase_fail_count, 1U);
        s_retained_nvs_telemetry.last_error = ret;
        store->last_error = ret;
    }
    portEXIT_CRITICAL(&s_store_state_mux);
}

static bool sd_error_latches_degraded(esp_err_t ret)
{
    return ret != ESP_OK &&
           ret != ESP_ERR_INVALID_ARG &&
           ret != ESP_ERR_INVALID_SIZE &&
           ret != ESP_ERR_NOT_FOUND;
}

static void note_sd_failure(const d1l_retained_blob_store_config_t *config,
                            d1l_retained_blob_store_sd_op_t op,
                            esp_err_t ret)
{
    if (!config || config->id >= D1L_RETAINED_BLOB_STORE_COUNT ||
        ret == ESP_OK || ret == ESP_ERR_INVALID_ARG) {
        return;
    }

    portENTER_CRITICAL(&s_store_state_mux);
    d1l_retained_blob_store_sd_stats_t *stats = &s_store_sd_stats[config->id];
    switch (op) {
    case D1L_RETAINED_SD_OP_READ:
        stats->sd_read_fail_count++;
        break;
    case D1L_RETAINED_SD_OP_WRITE:
        stats->sd_write_fail_count++;
        break;
    case D1L_RETAINED_SD_OP_RENAME:
        stats->sd_rename_fail_count++;
        break;
    default:
        break;
    }
    stats->sd_last_error = ret;
    if (sd_error_latches_degraded(ret)) {
        stats->sd_degraded_latched = true;
    }
    portEXIT_CRITICAL(&s_store_state_mux);
}

static void note_nvs_mirror_failure(const d1l_retained_blob_store_config_t *config,
                                    esp_err_t ret)
{
    if (!config || config->id >= D1L_RETAINED_BLOB_STORE_COUNT || ret == ESP_OK) {
        return;
    }
    portENTER_CRITICAL(&s_store_state_mux);
    d1l_retained_blob_store_sd_stats_t *stats = &s_store_sd_stats[config->id];
    stats->nvs_mirror_fail_count++;
    stats->nvs_mirror_last_error = ret;
    portEXIT_CRITICAL(&s_store_state_mux);
}

static const d1l_retained_blob_store_config_t *find_store(d1l_retained_blob_store_id_t store_id)
{
    for (size_t i = 0; i < sizeof(s_store_configs) / sizeof(s_store_configs[0]); ++i) {
        if (s_store_configs[i].id == store_id) {
            return &s_store_configs[i];
        }
    }
    return NULL;
}

static bool release_profile_allows_sd_history(void)
{
    return d1l_release_feature_available(
        D1L_RELEASE_FEATURE_SD_HISTORY);
}

static bool key_is_safe(const char *key)
{
    if (!key || key[0] == '\0') {
        return false;
    }
    for (const char *p = key; *p; ++p) {
        const char c = *p;
        const bool ok = (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool build_sd_path(const d1l_retained_blob_store_config_t *config,
                          const char *key,
                          const char *suffix,
                          char *out_path,
                          size_t out_path_size)
{
    if (!config || !config->sd_directory || !key_is_safe(key) || !suffix ||
        !out_path || out_path_size == 0) {
        return false;
    }
    const int written = snprintf(out_path, out_path_size, "%s/%s%s",
                                 config->sd_directory, key, suffix);
    return written > 0 && (size_t)written < out_path_size;
}

static bool copy_store_backend_state(
    const d1l_retained_blob_store_config_t *config,
    d1l_retained_blob_store_backend_state_t *out_state)
{
    if (!config || config->id >= D1L_RETAINED_BLOB_STORE_COUNT || !out_state) {
        return false;
    }
    const bool profile_allows_sd = release_profile_allows_sd_history();
    portENTER_CRITICAL(&s_store_state_mux);
    out_state->enabled =
        profile_allows_sd && s_store_sd_enabled[config->id];
    out_state->generation = s_store_backend_generation[config->id];
    portEXIT_CRITICAL(&s_store_state_mux);
    return true;
}

static bool store_backend_generation_matches(
    const d1l_retained_blob_store_config_t *config,
    uint32_t expected_generation)
{
    d1l_retained_blob_store_backend_state_t state = {0};
    return copy_store_backend_state(config, &state) && state.enabled &&
           state.generation == expected_generation;
}

static bool store_sd_enabled(const d1l_retained_blob_store_config_t *config)
{
    d1l_retained_blob_store_backend_state_t state = {0};
    return copy_store_backend_state(config, &state) && state.enabled;
}

static esp_err_t sd_media_lineage_ready(
    const d1l_retained_blob_store_config_t *config,
    uint32_t expected_backend_generation, bool *out_ready,
    bool *out_onboard_active, uint32_t *out_reset_generation,
    bool *out_marker_matches)
{
    if (!config || !out_ready || !out_onboard_active ||
        !out_reset_generation || !out_marker_matches) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_ready = false;
    *out_onboard_active = true;
    *out_reset_generation = 0U;
    *out_marker_matches = false;
    if (!store_backend_generation_matches(config,
                                          expected_backend_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = d1l_factory_reset_sd_lineage_snapshot(
        (d1l_factory_reset_sd_store_t)config->id, out_onboard_active,
        out_reset_generation);
    if (ret != ESP_OK) {
        return ret;
    }
    if (*out_reset_generation == 0U && !*out_onboard_active) {
        /* Upgrade compatibility: media predating the first factory reset has
         * no lineage domain yet. The first reset creates generation one and
         * thereafter every card must carry an exact marker. */
        *out_ready = true;
        *out_marker_matches = true;
        return ESP_OK;
    }
    if (*out_reset_generation == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    if (d1l_route_store_persistence_should_yield()) {
        return ESP_ERR_NOT_FINISHED;
    }

    char path[D1L_RP2040_FILE_PATH_MAX + 1U];
    if (!build_sd_path(config, D1L_RETAINED_SD_LINEAGE_MARKER_KEY,
                       ".bin", path, sizeof(path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    d1l_rp2040_file_result_t stat = {0};
    ret = d1l_rp2040_bridge_file_stat(
        path, &stat, D1L_RETAINED_SD_READ_TIMEOUT_MS);
    if (!store_backend_generation_matches(config,
                                          expected_backend_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_ERR_NOT_FOUND ||
        (ret == ESP_OK && (!stat.exists || stat.is_directory ||
                           stat.size != sizeof(
                               d1l_factory_reset_sd_media_marker_t)))) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_factory_reset_sd_media_marker_t marker = {0};
    d1l_rp2040_file_result_t read_result = {0};
    ret = d1l_rp2040_bridge_file_read(
        path, 0U, (uint8_t *)&marker, sizeof(marker), &read_result,
        D1L_RETAINED_SD_READ_TIMEOUT_MS);
    if (!store_backend_generation_matches(config,
                                          expected_backend_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    const bool marker_matches = read_result.length == sizeof(marker) &&
        d1l_factory_reset_sd_media_marker_matches(
            &marker, read_result.length,
            (d1l_factory_reset_sd_store_t)config->id,
            *out_reset_generation);
    *out_marker_matches = marker_matches;
    *out_ready = !*out_onboard_active && marker_matches;
    return ESP_OK;
}

static esp_err_t nvs_open_store(const d1l_retained_blob_store_config_t *config,
                                bool dedicated, nvs_open_mode_t open_mode,
                                nvs_handle_t *out_handle)
{
    if (!config || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return dedicated ?
        nvs_open_from_partition(D1L_RETAINED_NVS_PARTITION,
                                config->nvs_namespace, open_mode,
                                out_handle) :
        nvs_open(config->nvs_namespace, open_mode, out_handle);
}

static esp_err_t nvs_read_blob_from(const d1l_retained_blob_store_config_t *config,
                                    const char *key, void *dst,
                                    size_t *len_inout, bool dedicated)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open_store(config, dedicated, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : ret;
    }
    ret = nvs_get_blob(handle, key, dst, len_inout);
    nvs_close(handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    return ret;
}

static esp_err_t nvs_write_blob_to(const d1l_retained_blob_store_config_t *config,
                                   const char *key, const void *src, size_t len,
                                   bool dedicated)
{
    if (dedicated) {
        note_nvs_write_attempt(config, len);
    }
    nvs_handle_t handle;
    esp_err_t ret = nvs_open_store(config, dedicated, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        if (dedicated) {
            note_nvs_write_result(config, len, ret);
        }
        return ret;
    }

    /*
     * NVS commits are a finite endurance budget. Store workers already
     * coalesce dirty events, but a retry/reconcile pass can still present an
     * identical fallback snapshot. Treat that as a successful no-op instead
     * of issuing nvs_set_blob()/nvs_commit(). The existing telemetry keeps the
     * request in write_attempt_count while write_commit_count remains stable,
     * so callers can measure suppressed unchanged writes without pretending
     * that a flash commit occurred.
     *
     * Comparison is best effort: an allocation/read failure preserves the
     * prior write behavior instead of turning a recoverable commit into an
     * error. The compare and possible write share one NVS handle, keeping the
     * no-op decision inside the retained-store write boundary.
     */
    size_t existing_len = 0U;
    ret = nvs_get_blob(handle, key, NULL, &existing_len);
    if (ret == ESP_OK && existing_len == len) {
        void *existing = malloc(existing_len);
        if (existing) {
            size_t read_len = existing_len;
            ret = nvs_get_blob(handle, key, existing, &read_len);
            const bool unchanged = ret == ESP_OK && read_len == len &&
                memcmp(existing, src, len) == 0;
            free(existing);
            if (unchanged) {
                nvs_close(handle);
                return ESP_OK;
            }
        }
    }

    ret = nvs_set_blob(handle, key, src, len);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (dedicated) {
        note_nvs_write_result(config, len, ret);
    }
    return ret;
}

static esp_err_t nvs_erase_blob_from(const d1l_retained_blob_store_config_t *config,
                                     const char *key, bool dedicated)
{
    size_t existing_len = 0U;
    esp_err_t ret = nvs_read_blob_from(config, key, NULL, &existing_len,
                                       dedicated);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    if (dedicated) {
        note_nvs_erase_attempt(config);
    }
    nvs_handle_t handle;
    ret = nvs_open_store(config, dedicated, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        if (dedicated) {
            note_nvs_erase_result(config, ret);
        }
        return ret;
    }
    ret = nvs_erase_key(handle, key);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (dedicated) {
        note_nvs_erase_result(config, ret);
    }
    return ret;
}

/* Upgrade reads prefer the dedicated retained-data partition. If a key exists
 * only in the historical default NVS partition, copy it first and erase that
 * one scoped legacy key only after the dedicated commit succeeds. */
static esp_err_t nvs_read_blob(const d1l_retained_blob_store_config_t *config,
                               const char *key, void *dst, size_t *len_inout)
{
    if (!s_retained_nvs_ready) {
        return retained_nvs_unavailable_error();
    }

    const size_t requested_len = *len_inout;
    esp_err_t ret = nvs_read_blob_from(config, key, dst, len_inout, true);
    if (ret != ESP_ERR_NOT_FOUND) {
        return ret;
    }

    *len_inout = requested_len;
    ret = nvs_read_blob_from(config, key, dst, len_inout, false);
    if (ret != ESP_OK) {
        return ret;
    }
    const esp_err_t migrate_ret = nvs_write_blob_to(
        config, key, dst, *len_inout, true);
    if (migrate_ret == ESP_OK) {
        (void)nvs_erase_blob_from(config, key, false);
    }
    return ESP_OK;
}

static esp_err_t nvs_write_blob(const d1l_retained_blob_store_config_t *config,
                                const char *key, const void *src, size_t len)
{
    if (!s_retained_nvs_ready) {
        return retained_nvs_unavailable_error();
    }
    const esp_err_t ret = nvs_write_blob_to(config, key, src, len, true);
    if (ret == ESP_OK) {
        (void)nvs_erase_blob_from(config, key, false);
    }
    return ret;
}

static esp_err_t nvs_erase_blob(const d1l_retained_blob_store_config_t *config,
                                const char *key)
{
    if (!s_retained_nvs_ready) {
        return retained_nvs_unavailable_error();
    }
    /* Clear the obsolete default-NVS copy first. If power fails or the second
     * erase fails, the authoritative dedicated copy remains and no stale
     * legacy value can be migrated back as a successful clear. */
    const esp_err_t legacy_ret = nvs_erase_blob_from(config, key, false);
    if (legacy_ret != ESP_OK) {
        return legacy_ret;
    }
    return nvs_erase_blob_from(config, key, true);
}

static bool retained_nvs_marker_structurally_valid(
    const d1l_retained_nvs_marker_t *marker)
{
    return marker &&
           marker->magic == D1L_RETAINED_NVS_META_MAGIC &&
           marker->magic_inverse == ~D1L_RETAINED_NVS_META_MAGIC &&
           marker->version_inverse == ~marker->version;
}

static bool retained_nvs_marker_valid(const d1l_retained_nvs_marker_t *marker)
{
    return retained_nvs_marker_structurally_valid(marker) &&
           marker->version == D1L_RETAINED_NVS_META_VERSION;
}

static bool retained_nvs_legacy_marker_valid(
    const d1l_retained_nvs_marker_t *marker)
{
    return retained_nvs_marker_structurally_valid(marker) &&
           marker->version == D1L_RETAINED_NVS_META_LEGACY_VERSION;
}

static bool retained_nvs_marker_erased(const d1l_retained_nvs_marker_t *marker)
{
    if (!marker) {
        return false;
    }
    const uint8_t *bytes = (const uint8_t *)marker;
    for (size_t i = 0U; i < sizeof(*marker); ++i) {
        if (bytes[i] != 0xffU) {
            return false;
        }
    }
    return true;
}

static esp_err_t retained_nvs_partition_erased(
    const esp_partition_t *partition, bool *out_erased)
{
    if (!partition || !out_erased) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t chunk[256];
    *out_erased = false;
    for (size_t offset = 0U; offset < partition->size;
         offset += sizeof(chunk)) {
        const size_t remaining = partition->size - offset;
        const size_t read_size = remaining < sizeof(chunk) ?
            remaining : sizeof(chunk);
        const esp_err_t ret = esp_partition_read(partition, offset,
                                                  chunk, read_size);
        if (ret != ESP_OK) {
            return ret;
        }
        for (size_t i = 0U; i < read_size; ++i) {
            if (chunk[i] != 0xffU) {
                return ESP_OK;
            }
        }
    }
    *out_erased = true;
    return ESP_OK;
}

static d1l_retained_nvs_marker_t retained_nvs_marker_value(void)
{
    return (d1l_retained_nvs_marker_t) {
        .magic = D1L_RETAINED_NVS_META_MAGIC,
        .version = D1L_RETAINED_NVS_META_VERSION,
        .magic_inverse = ~D1L_RETAINED_NVS_META_MAGIC,
        .version_inverse = ~D1L_RETAINED_NVS_META_VERSION,
    };
}

static esp_err_t read_default_retained_nvs_sentinel(bool *out_present)
{
    if (!out_present) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_present = false;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(D1L_RETAINED_NVS_SENTINEL_NAMESPACE,
                             NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_retained_nvs_marker_t sentinel = {0};
    size_t sentinel_len = sizeof(sentinel);
    ret = nvs_get_blob(handle, D1L_RETAINED_NVS_SENTINEL_KEY,
                       &sentinel, &sentinel_len);
    nvs_close(handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK || sentinel_len != sizeof(sentinel) ||
        !retained_nvs_marker_valid(&sentinel)) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    *out_present = true;
    return ESP_OK;
}

static esp_err_t ensure_default_retained_nvs_sentinel(void)
{
    bool present = false;
    esp_err_t ret = read_default_retained_nvs_sentinel(&present);
    if (ret != ESP_OK) {
        return ret;
    }
    if (present) {
        s_retained_nvs_sentinel_ready = true;
        return ESP_OK;
    }

    nvs_handle_t handle;
    ret = nvs_open(D1L_RETAINED_NVS_SENTINEL_NAMESPACE,
                   NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    const d1l_retained_nvs_marker_t sentinel = retained_nvs_marker_value();
    ret = nvs_set_blob(handle, D1L_RETAINED_NVS_SENTINEL_KEY,
                       &sentinel, sizeof(sentinel));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret == ESP_OK) {
        s_retained_nvs_sentinel_ready = true;
    }
    return ret;
}

static esp_err_t read_retained_nvs_anchor(bool *out_present)
{
    if (!out_present) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_present = false;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open_from_partition(
        D1L_RETAINED_NVS_PARTITION, D1L_RETAINED_NVS_SENTINEL_NAMESPACE,
        NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    d1l_retained_nvs_marker_t anchor = {0};
    size_t anchor_len = sizeof(anchor);
    ret = nvs_get_blob(handle, D1L_RETAINED_NVS_ANCHOR_KEY,
                       &anchor, &anchor_len);
    nvs_close(handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK || anchor_len != sizeof(anchor) ||
        !retained_nvs_marker_valid(&anchor)) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    *out_present = true;
    return ESP_OK;
}

static esp_err_t require_retained_nvs_anchor(void)
{
    bool present = false;
    const esp_err_t ret = read_retained_nvs_anchor(&present);
    if (ret == ESP_OK && present) {
        s_retained_nvs_anchor_ready = true;
        return ESP_OK;
    }
    return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
}

static esp_err_t ensure_retained_nvs_anchor(void)
{
    bool present = false;
    esp_err_t ret = read_retained_nvs_anchor(&present);
    if (ret != ESP_OK) {
        return ret;
    }
    if (present) {
        s_retained_nvs_anchor_ready = true;
        return ESP_OK;
    }

    nvs_handle_t handle;
    ret = nvs_open_from_partition(
        D1L_RETAINED_NVS_PARTITION, D1L_RETAINED_NVS_SENTINEL_NAMESPACE,
        NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    const d1l_retained_nvs_marker_t anchor = retained_nvs_marker_value();
    ret = nvs_set_blob(handle, D1L_RETAINED_NVS_ANCHOR_KEY,
                       &anchor, sizeof(anchor));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret == ESP_OK) {
        s_retained_nvs_anchor_ready = true;
    }
    return ret;
}

static esp_err_t prepare_retained_nvs_markers(
    const esp_partition_t *meta_partition,
    const d1l_retained_nvs_marker_t *marker_first,
    const d1l_retained_nvs_marker_t *marker_last,
    bool initialize_blank_nvs)
{
    if (!meta_partition || !marker_first || !marker_last) {
        return ESP_ERR_INVALID_ARG;
    }
    const d1l_retained_nvs_marker_t marker = retained_nvs_marker_value();
    esp_err_t ret = ESP_OK;
    if (!retained_nvs_marker_erased(marker_first) ||
        !retained_nvs_marker_erased(marker_last)) {
        ret = esp_partition_erase_range(meta_partition, 0U,
                                        meta_partition->size);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    ret = esp_partition_write(meta_partition, 0U, &marker, sizeof(marker));
    if (ret != ESP_OK) {
        return ret;
    }
    s_retained_nvs_marker_ready = true;
    if (initialize_blank_nvs) {
        ret = nvs_flash_init_partition(D1L_RETAINED_NVS_PARTITION);
        if (ret != ESP_OK) {
            return ret;
        }
        s_retained_nvs_initialized_this_boot = true;
    }
    ret = ensure_retained_nvs_anchor();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_partition_write(meta_partition,
                              meta_partition->size - sizeof(marker),
                              &marker, sizeof(marker));
    if (ret == ESP_OK) {
        s_retained_nvs_markers_complete = true;
    }
    return ret;
}

static esp_err_t initialize_retained_nvs_partition(void)
{
    const esp_partition_t *meta_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        D1L_RETAINED_NVS_META_PARTITION);
    const esp_partition_t *nvs_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
        D1L_RETAINED_NVS_PARTITION);
    if (!meta_partition || !nvs_partition ||
        meta_partition->size < (2U * sizeof(d1l_retained_nvs_marker_t)) ||
        nvs_partition->size == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    d1l_retained_nvs_marker_t marker_first = {0};
    d1l_retained_nvs_marker_t marker_last = {0};
    esp_err_t ret = esp_partition_read(meta_partition, 0U,
                                       &marker_first, sizeof(marker_first));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_partition_read(meta_partition,
                             meta_partition->size - sizeof(marker_last),
                             &marker_last, sizeof(marker_last));
    if (ret != ESP_OK) {
        return ret;
    }

    const d1l_retained_nvs_marker_t marker = retained_nvs_marker_value();
    bool sentinel_present = false;
    ret = read_default_retained_nvs_sentinel(&sentinel_present);
    if (ret != ESP_OK) {
        return ret;
    }
    s_retained_nvs_sentinel_ready = sentinel_present;
    const bool marker_first_valid = retained_nvs_marker_valid(&marker_first);
    const bool marker_last_valid = retained_nvs_marker_valid(&marker_last);
    const bool marker_first_legacy =
        retained_nvs_legacy_marker_valid(&marker_first);
    const bool marker_last_legacy =
        retained_nvs_legacy_marker_valid(&marker_last);
    if ((retained_nvs_marker_structurally_valid(&marker_first) &&
         !marker_first_valid && !marker_first_legacy) ||
        (retained_nvs_marker_structurally_valid(&marker_last) &&
         !marker_last_valid && !marker_last_legacy)) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((marker_first_valid || marker_last_valid) &&
        (marker_first_legacy || marker_last_legacy) && !sentinel_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (marker_first_valid || marker_last_valid) {
        /* A completely blank retained region is a legitimate power-loss
         * resume only after the first marker and before NVS initialization.
         * Every later durable state has either the dedicated anchor, the
         * second marker, or the default sentinel and must fail closed if the
         * retained region is subsequently erased. */
        const bool blank_resume_allowed =
            marker_first_valid && retained_nvs_marker_erased(&marker_last) &&
            !sentinel_present;
        if (!blank_resume_allowed) {
            bool established_partition_erased = false;
            ret = retained_nvs_partition_erased(
                nvs_partition, &established_partition_erased);
            if (ret != ESP_OK || established_partition_erased) {
                return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
            }
        }
        s_retained_nvs_marker_ready = true;
        s_retained_nvs_markers_complete =
            marker_first_valid && marker_last_valid;
        ret = nvs_flash_init_partition(D1L_RETAINED_NVS_PARTITION);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = blank_resume_allowed ? ensure_retained_nvs_anchor() :
                                     require_retained_nvs_anchor();
        if (ret != ESP_OK) {
            return ret;
        }
        if (marker_first_valid &&
            retained_nvs_marker_erased(&marker_last)) {
            ret = esp_partition_write(
                meta_partition,
                meta_partition->size - sizeof(marker),
                &marker, sizeof(marker));
            if (ret == ESP_OK) {
                s_retained_nvs_markers_complete = true;
            }
            return ret;
        }
        if (marker_last_valid &&
            retained_nvs_marker_erased(&marker_first)) {
            ret = esp_partition_write(meta_partition, 0U,
                                      &marker, sizeof(marker));
            if (ret == ESP_OK) {
                s_retained_nvs_markers_complete = true;
            }
            return ret;
        }
        /* A non-erased corrupt companion cannot be repaired in place. Wait
         * until the external sentinel is durable, then rebuild both marker
         * slots around the already-required anchor. If power fails during
         * this metadata-sector repair, sentinel-owned recovery is still
         * non-destructive. */
        if (sentinel_present && !(marker_first_valid && marker_last_valid)) {
            return prepare_retained_nvs_markers(
                meta_partition, &marker_first, &marker_last, false);
        }
        s_retained_nvs_marker_finalize_pending =
            !s_retained_nvs_markers_complete;
        return ESP_OK;
    }

    /* Version-1 markers belong to the published pre-anchor candidate. They
     * prove ownership even when its empty NVS never programmed a page. Keep
     * those markers in place while initializing and committing the anchor;
     * migration and the default sentinel complete before v2 rewrites meta. */
    if (marker_first_legacy || marker_last_legacy) {
        s_retained_nvs_marker_ready = true;
        ret = nvs_flash_init_partition(D1L_RETAINED_NVS_PARTITION);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = ensure_retained_nvs_anchor();
        if (ret != ESP_OK) {
            return ret;
        }
        s_retained_nvs_marker_finalize_pending = true;
        return ESP_OK;
    }

    bool partition_erased = false;
    ret = retained_nvs_partition_erased(nvs_partition, &partition_erased);
    if (ret != ESP_OK) {
        return ret;
    }
    if (partition_erased) {
        if (sentinel_present) {
            return ESP_ERR_INVALID_STATE;
        }
        return prepare_retained_nvs_markers(
            meta_partition, &marker_first, &marker_last, true);
    }

    /* Only the default-NVS sentinel proves an unmarked region belongs to the
     * retained store. NVS initialization is not a read-only probe: ESP-IDF
     * can erase a corrupt page while activating it. With sentinel ownership,
     * require the already-committed dedicated anchor before rebuilding the
     * metadata markers, and never explicitly erase on a recovery failure. */
    if (sentinel_present) {
        ret = nvs_flash_init_partition(D1L_RETAINED_NVS_PARTITION);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = require_retained_nvs_anchor();
        if (ret != ESP_OK) {
            return ret;
        }
        return prepare_retained_nvs_markers(
            meta_partition, &marker_first, &marker_last, false);
    }

    /* Nonblank bytes with no ownership claim are ambiguous. Firmware never
     * probes or erases them: an installer must first verify the predecessor
     * layout and perform the separately audited, scoped partition erase. */
    s_retained_nvs_external_init_required = true;
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t finalize_retained_nvs_markers(void)
{
    const esp_partition_t *meta_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        D1L_RETAINED_NVS_META_PARTITION);
    if (!meta_partition ||
        meta_partition->size < (2U * sizeof(d1l_retained_nvs_marker_t))) {
        return ESP_ERR_NOT_FOUND;
    }
    d1l_retained_nvs_marker_t marker_first = {0};
    d1l_retained_nvs_marker_t marker_last = {0};
    esp_err_t ret = esp_partition_read(meta_partition, 0U,
                                       &marker_first, sizeof(marker_first));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_partition_read(meta_partition,
                             meta_partition->size - sizeof(marker_last),
                             &marker_last, sizeof(marker_last));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = prepare_retained_nvs_markers(
        meta_partition, &marker_first, &marker_last, false);
    if (ret == ESP_OK) {
        s_retained_nvs_marker_finalize_pending = false;
    }
    return ret;
}

static esp_err_t retained_nvs_unavailable_error(void)
{
    if (s_retained_nvs_error != ESP_OK) {
        return s_retained_nvs_error;
    }
    if (s_retained_nvs_migration_error != ESP_OK) {
        return s_retained_nvs_migration_error;
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t migrate_legacy_nvs_key(
    const d1l_retained_blob_store_config_t *config, const char *key)
{
    size_t dedicated_len = 0U;
    esp_err_t ret = nvs_read_blob_from(config, key, NULL, &dedicated_len, true);
    if (ret == ESP_OK) {
        size_t legacy_len = 0U;
        ret = nvs_read_blob_from(config, key, NULL, &legacy_len, false);
        if (ret == ESP_ERR_NOT_FOUND) {
            return ESP_OK;
        }
        if (ret != ESP_OK) {
            return ret;
        }
        if (dedicated_len == 0U || dedicated_len != legacy_len ||
            dedicated_len > D1L_RETAINED_NVS_MIGRATION_MAX_BYTES) {
            return ESP_ERR_INVALID_STATE;
        }

        uint8_t *dedicated_blob = malloc(dedicated_len);
        uint8_t *legacy_blob = malloc(legacy_len);
        if (!dedicated_blob || !legacy_blob) {
            free(dedicated_blob);
            free(legacy_blob);
            return ESP_ERR_NO_MEM;
        }
        size_t dedicated_read_len = dedicated_len;
        size_t legacy_read_len = legacy_len;
        ret = nvs_read_blob_from(config, key, dedicated_blob,
                                 &dedicated_read_len, true);
        if (ret == ESP_OK) {
            ret = nvs_read_blob_from(config, key, legacy_blob,
                                     &legacy_read_len, false);
        }
        const bool copies_match =
            ret == ESP_OK && dedicated_read_len == dedicated_len &&
            legacy_read_len == legacy_len &&
            memcmp(dedicated_blob, legacy_blob, dedicated_len) == 0;
        free(dedicated_blob);
        free(legacy_blob);
        if (ret != ESP_OK) {
            return ret;
        }
        if (!copies_match) {
            return ESP_ERR_INVALID_STATE;
        }
        return nvs_erase_blob_from(config, key, false);
    }
    if (ret != ESP_ERR_NOT_FOUND) {
        return ret;
    }

    size_t legacy_len = 0U;
    ret = nvs_read_blob_from(config, key, NULL, &legacy_len, false);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (legacy_len == 0U || legacy_len > D1L_RETAINED_NVS_MIGRATION_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *blob = malloc(legacy_len);
    if (!blob) {
        return ESP_ERR_NO_MEM;
    }
    size_t read_len = legacy_len;
    ret = nvs_read_blob_from(config, key, blob, &read_len, false);
    if (ret == ESP_OK && read_len == legacy_len) {
        ret = nvs_write_blob_to(config, key, blob, legacy_len, true);
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    free(blob);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_erase_blob_from(config, key, false);
    if (ret == ESP_OK) {
        s_retained_nvs_migrated_keys++;
    }
    return ret;
}

static esp_err_t migrate_known_legacy_nvs_keys(void)
{
    static const struct {
        d1l_retained_blob_store_id_t store_id;
        const char *key;
    } keys[] = {
        {D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public"},
        {D1L_RETAINED_BLOB_STORE_DM_MESSAGES, "threads"},
        {D1L_RETAINED_BLOB_STORE_ROUTES, "routes_v2"},
        {D1L_RETAINED_BLOB_STORE_ROUTES, "routes"},
        {D1L_RETAINED_BLOB_STORE_PACKET_LOG, "ring"},
    };

    s_retained_nvs_migrated_keys = 0U;
    s_retained_nvs_migration_error = ESP_OK;
    for (size_t i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        const d1l_retained_blob_store_config_t *config =
            find_store(keys[i].store_id);
        const esp_err_t ret = migrate_legacy_nvs_key(config, keys[i].key);
        if (ret != ESP_OK) {
            s_retained_nvs_migration_error = ret;
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t sd_read_blob(const d1l_retained_blob_store_config_t *config,
                              const char *key,
                              void *dst,
                              size_t *len_inout)
{
    if (!config || !key || !dst || !len_inout) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_retained_blob_store_backend_state_t backend = {0};
    if (!copy_store_backend_state(config, &backend) || !backend.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    bool lineage_ready = false;
    bool lineage_active = true;
    bool marker_matches = false;
    uint32_t lineage_generation = 0U;
    const esp_err_t lineage_ret = sd_media_lineage_ready(
        config, backend.generation, &lineage_ready, &lineage_active,
        &lineage_generation, &marker_matches);
    (void)lineage_active;
    (void)lineage_generation;
    (void)marker_matches;
    if (lineage_ret != ESP_OK) {
        return lineage_ret;
    }
    if (!lineage_ready) {
        return ESP_ERR_NOT_FOUND;
    }
    char path[D1L_RP2040_FILE_PATH_MAX + 1U];
    if (!build_sd_path(config, key, ".bin", path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }

    if (d1l_route_store_persistence_should_yield()) {
        return ESP_ERR_NOT_FINISHED;
    }
    d1l_rp2040_file_result_t stat = {0};
    esp_err_t ret = d1l_rp2040_bridge_file_stat(path, &stat, D1L_RETAINED_SD_READ_TIMEOUT_MS);
    if (!store_backend_generation_matches(config, backend.generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (!stat.exists) {
        return ESP_ERR_NOT_FOUND;
    }
    if (stat.is_directory) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stat.size > *len_inout) {
        *len_inout = stat.size;
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *out = (uint8_t *)dst;
    size_t offset = 0;
    while (offset < stat.size) {
        /* A completed bridge chunk is a transaction-safe yield boundary.
         * Foreground owners never see this cancellation because the
         * predicate is scoped to the background retained worker task. */
        if (d1l_route_store_persistence_should_yield()) {
            return ESP_ERR_NOT_FINISHED;
        }
        if (!store_backend_generation_matches(config, backend.generation)) {
            return ESP_ERR_INVALID_STATE;
        }
        const size_t remaining = stat.size - offset;
        const size_t chunk = remaining > D1L_RP2040_FILE_CHUNK_MAX ?
                             D1L_RP2040_FILE_CHUNK_MAX : remaining;
        d1l_rp2040_file_result_t read_result = {0};
        ret = d1l_rp2040_bridge_file_read(path, (uint32_t)offset,
                                          out + offset, chunk, &read_result,
                                          D1L_RETAINED_SD_READ_TIMEOUT_MS);
        if (!store_backend_generation_matches(config, backend.generation)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (ret != ESP_OK) {
            return ret;
        }
        if (read_result.length == 0 && !read_result.eof) {
            return ESP_FAIL;
        }
        offset += read_result.length;
        if (read_result.eof && offset < stat.size) {
            return ESP_FAIL;
        }
    }
    *len_inout = offset;
    return ESP_OK;
}

static esp_err_t sd_write_blob_for_generation(
    const d1l_retained_blob_store_config_t *config,
    const char *key,
    const void *src,
    size_t len,
    uint32_t expected_generation)
{
    char path[D1L_RP2040_FILE_PATH_MAX + 1U];
    char temp_path[D1L_RP2040_FILE_PATH_MAX + 1U];
    if (!build_sd_path(config, key, ".bin", path, sizeof(path)) ||
        !build_sd_path(config, key, ".tmp", temp_path, sizeof(temp_path)) ||
        !src || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_backend_generation_matches(config, expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t *data = (const uint8_t *)src;
    size_t offset = 0;
    while (offset < len) {
        /* Temp-file chunks are non-destructive until the final rename. A
         * pending foreground quiesce may leave a partial temp file, which a
         * later offset-zero retry safely truncates. */
        if (d1l_route_store_persistence_should_yield()) {
            return ESP_ERR_NOT_FINISHED;
        }
        const size_t remaining = len - offset;
        const size_t chunk = remaining > D1L_RP2040_FILE_CHUNK_MAX ?
                             D1L_RP2040_FILE_CHUNK_MAX : remaining;
        d1l_rp2040_file_result_t write_result = {0};
        esp_err_t ret = d1l_rp2040_bridge_file_write(temp_path, (uint32_t)offset,
                                                     data + offset, chunk,
                                                     offset == 0, &write_result,
                                                     D1L_RETAINED_SD_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK || write_result.length != chunk) {
            const esp_err_t failure = ret == ESP_OK ? ESP_FAIL : ret;
            note_sd_failure(config, D1L_RETAINED_SD_OP_WRITE, failure);
            if (store_backend_generation_matches(config, expected_generation)) {
                d1l_rp2040_file_result_t ignored = {0};
                (void)d1l_rp2040_bridge_file_delete(
                    temp_path, &ignored, D1L_RETAINED_SD_WRITE_TIMEOUT_MS);
            }
            return failure;
        }
        offset += chunk;
    }

    /* The replace-rename is the destructive commit point. If media changed
     * while chunks were written, leave the temp path alone: it may now belong
     * to the replacement card, and the old primary must remain untouched. */
    if (d1l_route_store_persistence_should_yield()) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (!store_backend_generation_matches(config, expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_rp2040_file_result_t rename_result = {0};
    esp_err_t ret = d1l_rp2040_bridge_file_rename(temp_path, path, true, &rename_result,
                                                  D1L_RETAINED_SD_WRITE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        note_sd_failure(config, D1L_RETAINED_SD_OP_RENAME, ret);
    }
    return ret;
}

static esp_err_t sd_write_blob(const d1l_retained_blob_store_config_t *config,
                               const char *key,
                               const void *src,
                               size_t len)
{
    d1l_retained_blob_store_backend_state_t state = {0};
    if (!copy_store_backend_state(config, &state) || !state.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    return sd_write_blob_with_lineage(config, key, src, len,
                                      state.generation);
}

static esp_err_t sd_erase_blob_for_generation(
    const d1l_retained_blob_store_config_t *config,
    const char *key,
    uint32_t expected_generation)
{
    char path[D1L_RP2040_FILE_PATH_MAX + 1U];
    char temp_path[D1L_RP2040_FILE_PATH_MAX + 1U];
    if (!build_sd_path(config, key, ".bin", path, sizeof(path)) ||
        !build_sd_path(config, key, ".tmp", temp_path, sizeof(temp_path))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_backend_generation_matches(config, expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }

    d1l_rp2040_file_result_t result = {0};
    esp_err_t ret = d1l_rp2040_bridge_file_delete(path, &result,
                                                  D1L_RETAINED_SD_WRITE_TIMEOUT_MS);
    if (ret == ESP_ERR_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (!store_backend_generation_matches(config, expected_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_rp2040_file_result_t temp_result = {0};
    esp_err_t temp_ret = d1l_rp2040_bridge_file_delete(temp_path, &temp_result,
                                                       D1L_RETAINED_SD_WRITE_TIMEOUT_MS);
    if (temp_ret == ESP_ERR_NOT_FOUND) {
        temp_ret = ESP_OK;
    }
    return ret == ESP_OK ? temp_ret : ret;
}

static bool sd_lineage_key_allowed(
    const d1l_retained_blob_store_config_t *config, const char *key)
{
    if (!config || !key) {
        return false;
    }
    switch (config->id) {
    case D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES:
        return strcmp(key, "public") == 0;
    case D1L_RETAINED_BLOB_STORE_DM_MESSAGES:
        return strcmp(key, "threads") == 0;
    case D1L_RETAINED_BLOB_STORE_ROUTES:
        return strcmp(key, "routes_v2") == 0 || strcmp(key, "routes") == 0;
    case D1L_RETAINED_BLOB_STORE_PACKET_LOG:
        return strcmp(key, "ring") == 0;
    default:
        return false;
    }
}

static esp_err_t sd_purge_store_for_generation(
    const d1l_retained_blob_store_config_t *config,
    uint32_t expected_generation)
{
    static const char *const public_keys[] = {"public"};
    static const char *const dm_keys[] = {"threads"};
    static const char *const route_keys[] = {"routes_v2", "routes"};
    static const char *const packet_keys[] = {"ring"};
    const char *const *keys = NULL;
    size_t key_count = 0U;
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (config->id) {
    case D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES:
        keys = public_keys;
        key_count = sizeof(public_keys) / sizeof(public_keys[0]);
        break;
    case D1L_RETAINED_BLOB_STORE_DM_MESSAGES:
        keys = dm_keys;
        key_count = sizeof(dm_keys) / sizeof(dm_keys[0]);
        break;
    case D1L_RETAINED_BLOB_STORE_ROUTES:
        keys = route_keys;
        key_count = sizeof(route_keys) / sizeof(route_keys[0]);
        break;
    case D1L_RETAINED_BLOB_STORE_PACKET_LOG:
        keys = packet_keys;
        key_count = sizeof(packet_keys) / sizeof(packet_keys[0]);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < key_count; ++i) {
        const esp_err_t ret = sd_erase_blob_for_generation(
            config, keys[i], expected_generation);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t sd_finalize_lineage_for_generation(
    const d1l_retained_blob_store_config_t *config,
    uint32_t lineage_generation, uint32_t expected_backend_generation)
{
    if (!store_backend_generation_matches(config,
                                          expected_backend_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    d1l_factory_reset_sd_media_marker_t marker = {0};
    const d1l_factory_reset_sd_store_t reset_store =
        (d1l_factory_reset_sd_store_t)config->id;
    d1l_factory_reset_sd_media_marker_init(
        &marker, reset_store, lineage_generation);
    esp_err_t ret = sd_write_blob_for_generation(
        config, D1L_RETAINED_SD_LINEAGE_MARKER_KEY, &marker,
        sizeof(marker), expected_backend_generation);
    if (ret != ESP_OK) {
        return ret;
    }

    /* The marker rename is followed by an independent readback while the
     * onboard fence is still active. A power cut or card swap before this
     * point therefore cannot expose the media. */
    bool ready = false;
    bool onboard_active = true;
    bool marker_matches = false;
    uint32_t readback_generation = 0U;
    ret = sd_media_lineage_ready(
        config, expected_backend_generation, &ready, &onboard_active,
        &readback_generation, &marker_matches);
    (void)ready;
    if (ret != ESP_OK || !marker_matches ||
        readback_generation != lineage_generation) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    if (onboard_active) {
        ret = d1l_factory_reset_sd_lineage_clear(
            reset_store, lineage_generation);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    /* A post-clear card swap is still safe: every read independently requires
     * an exact marker on the currently mounted backend generation. */
    return store_backend_generation_matches(config,
                                            expected_backend_generation) ?
        ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t sd_write_blob_with_lineage(
    const d1l_retained_blob_store_config_t *config, const char *key,
    const void *src, size_t len, uint32_t expected_generation)
{
    if (!config || !sd_lineage_key_allowed(config, key) || !src || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    bool ready = false;
    bool active = true;
    bool marker_matches = false;
    uint32_t lineage_generation = 0U;
    esp_err_t ret = sd_media_lineage_ready(
        config, expected_generation, &ready, &active,
        &lineage_generation, &marker_matches);
    (void)active;
    (void)marker_matches;
    if (ret != ESP_OK) {
        return ret;
    }
    if (ready) {
        return sd_write_blob_for_generation(config, key, src, len,
                                            expected_generation);
    }
    if (config->id == D1L_RETAINED_BLOB_STORE_PACKET_LOG) {
        /* Packet history has 64 segment aliases. packet_log.c must delete
         * those first and then use the dedicated completion helper below. */
        return ESP_ERR_INVALID_STATE;
    }
    ret = sd_purge_store_for_generation(config, expected_generation);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = sd_write_blob_for_generation(config, key, src, len,
                                       expected_generation);
    if (ret != ESP_OK) {
        return ret;
    }
    return sd_finalize_lineage_for_generation(
        config, lineage_generation, expected_generation);
}

static esp_err_t sd_erase_blob_with_lineage(
    const d1l_retained_blob_store_config_t *config, const char *key,
    uint32_t expected_generation)
{
    if (!config || !sd_lineage_key_allowed(config, key)) {
        return ESP_ERR_INVALID_ARG;
    }
    bool ready = false;
    bool active = true;
    bool marker_matches = false;
    uint32_t lineage_generation = 0U;
    esp_err_t ret = sd_media_lineage_ready(
        config, expected_generation, &ready, &active,
        &lineage_generation, &marker_matches);
    (void)active;
    (void)marker_matches;
    if (ret != ESP_OK) {
        return ret;
    }
    if (ready) {
        return sd_erase_blob_for_generation(config, key,
                                            expected_generation);
    }
    if (config->id == D1L_RETAINED_BLOB_STORE_PACKET_LOG) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = sd_purge_store_for_generation(config, expected_generation);
    if (ret != ESP_OK) {
        return ret;
    }
    return sd_finalize_lineage_for_generation(
        config, lineage_generation, expected_generation);
}

static esp_err_t sd_erase_blob(const d1l_retained_blob_store_config_t *config,
                               const char *key)
{
    d1l_retained_blob_store_backend_state_t state = {0};
    if (!copy_store_backend_state(config, &state) || !state.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    return sd_erase_blob_with_lineage(config, key, state.generation);
}

const char *d1l_retained_blob_store_backend_name(d1l_retained_blob_store_id_t store_id)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config) {
        return "unavailable";
    }
    if (store_sd_enabled(config)) {
        return "sd";
    }
    return s_retained_nvs_ready ? "nvs" : "unavailable";
}

bool d1l_retained_blob_store_is_available(d1l_retained_blob_store_id_t store_id)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    return config && (store_sd_enabled(config) || s_retained_nvs_ready);
}

bool d1l_retained_blob_store_uses_sd(d1l_retained_blob_store_id_t store_id)
{
    d1l_retained_blob_store_backend_state_t state = {0};
    return d1l_retained_blob_store_backend_state(store_id, &state) && state.enabled;
}

bool d1l_retained_blob_store_backend_state(
    d1l_retained_blob_store_id_t store_id,
    d1l_retained_blob_store_backend_state_t *out_state)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !out_state) {
        return false;
    }
    return copy_store_backend_state(config, out_state);
}

bool d1l_retained_blob_store_sd_stats(d1l_retained_blob_store_id_t store_id,
                                      d1l_retained_blob_store_sd_stats_t *out_stats)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !out_stats) {
        return false;
    }
    portENTER_CRITICAL(&s_store_state_mux);
    *out_stats = s_store_sd_stats[config->id];
    portEXIT_CRITICAL(&s_store_state_mux);
    return true;
}

bool d1l_retained_blob_store_any_sd_degraded(void)
{
    bool degraded = false;
    portENTER_CRITICAL(&s_store_state_mux);
    for (size_t i = 0; i < D1L_RETAINED_BLOB_STORE_COUNT; ++i) {
        if (s_store_sd_stats[i].sd_degraded_latched) {
            degraded = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_store_state_mux);
    return degraded;
}

void d1l_retained_blob_store_note_sd_backend(bool data_ready,
                                             bool file_ops_supported,
                                             bool atomic_rename_supported,
                                             uint32_t file_line_max,
                                             uint32_t file_chunk_max,
                                             uint32_t path_max)
{
    const bool can_use_retained_sd =
        release_profile_allows_sd_history() &&
        data_ready &&
        file_ops_supported &&
        atomic_rename_supported &&
        file_line_max >= D1L_RP2040_FILE_LINE_MAX &&
        file_chunk_max >= D1L_RP2040_FILE_CHUNK_MAX &&
        path_max >= D1L_RP2040_FILE_PATH_MAX;

    portENTER_CRITICAL(&s_store_state_mux);
    for (size_t i = 0; i < D1L_RETAINED_BLOB_STORE_COUNT; ++i) {
        if (s_store_sd_enabled[i] != can_use_retained_sd) {
            s_store_sd_enabled[i] = can_use_retained_sd;
            if (s_store_backend_generation[i] < UINT32_MAX) {
                s_store_backend_generation[i]++;
            }
        }
    }
    portEXIT_CRITICAL(&s_store_state_mux);
}

esp_err_t d1l_retained_blob_store_init(void)
{
    portENTER_CRITICAL(&s_store_state_mux);
    memset(&s_retained_nvs_telemetry, 0, sizeof(s_retained_nvs_telemetry));
    portEXIT_CRITICAL(&s_store_state_mux);
    s_retained_nvs_ready = false;
    s_retained_nvs_marker_ready = false;
    s_retained_nvs_markers_complete = false;
    s_retained_nvs_anchor_ready = false;
    s_retained_nvs_sentinel_ready = false;
    s_retained_nvs_external_init_required = false;
    s_retained_nvs_marker_finalize_pending = false;
    s_retained_nvs_initialized_this_boot = false;
    s_retained_nvs_migrated_keys = 0U;
    s_retained_nvs_migration_error = ESP_OK;

    const esp_err_t ret = initialize_retained_nvs_partition();
    s_retained_nvs_error = ret;
    if (ret != ESP_OK) {
        s_retained_nvs_migration_error = ret;
        return ret;
    }

    esp_err_t migration_ret = migrate_known_legacy_nvs_keys();
    if (migration_ret == ESP_OK) {
        migration_ret = ensure_default_retained_nvs_sentinel();
        if (migration_ret != ESP_OK) {
            s_retained_nvs_migration_error = migration_ret;
        }
    }
    if (migration_ret == ESP_OK && s_retained_nvs_marker_finalize_pending) {
        migration_ret = finalize_retained_nvs_markers();
        if (migration_ret != ESP_OK) {
            s_retained_nvs_migration_error = migration_ret;
        }
    }
    s_retained_nvs_ready = migration_ret == ESP_OK;
    return migration_ret;
}

bool d1l_retained_blob_store_nvs_ready(void)
{
    return s_retained_nvs_ready;
}

esp_err_t d1l_retained_blob_store_nvs_error(void)
{
    return s_retained_nvs_error;
}

bool d1l_retained_blob_store_nvs_marker_ready(void)
{
    return s_retained_nvs_marker_ready;
}

bool d1l_retained_blob_store_nvs_markers_complete(void)
{
    return s_retained_nvs_markers_complete;
}

bool d1l_retained_blob_store_nvs_anchor_ready(void)
{
    return s_retained_nvs_anchor_ready;
}

bool d1l_retained_blob_store_nvs_sentinel_ready(void)
{
    return s_retained_nvs_sentinel_ready;
}

bool d1l_retained_blob_store_nvs_external_init_required(void)
{
    return s_retained_nvs_external_init_required;
}

bool d1l_retained_blob_store_nvs_initialized_this_boot(void)
{
    return s_retained_nvs_initialized_this_boot;
}

uint32_t d1l_retained_blob_store_nvs_migrated_keys(void)
{
    return s_retained_nvs_migrated_keys;
}

esp_err_t d1l_retained_blob_store_nvs_migration_error(void)
{
    return s_retained_nvs_migration_error;
}

bool d1l_retained_blob_store_nvs_telemetry(
    d1l_retained_blob_store_nvs_telemetry_t *out_telemetry)
{
    if (!out_telemetry) {
        return false;
    }

    portENTER_CRITICAL(&s_store_state_mux);
    *out_telemetry = s_retained_nvs_telemetry;
    portEXIT_CRITICAL(&s_store_state_mux);

    nvs_stats_t stats = {0};
    out_telemetry->capacity_error = s_retained_nvs_ready ?
        nvs_get_stats(D1L_RETAINED_NVS_PARTITION, &stats) :
        retained_nvs_unavailable_error();
    out_telemetry->capacity_valid = out_telemetry->capacity_error == ESP_OK;
    if (out_telemetry->capacity_valid) {
        out_telemetry->used_entries = stats.used_entries;
        out_telemetry->free_entries = stats.free_entries;
        out_telemetry->available_entries = stats.available_entries;
        out_telemetry->total_entries = stats.total_entries;
        out_telemetry->namespace_count = stats.namespace_count;
    }
    return true;
}

esp_err_t d1l_retained_blob_store_read_sd_primary(
    d1l_retained_blob_store_id_t store_id,
    const char *key,
    void *dst,
    size_t *len_inout)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !key || !dst || !len_inout) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_sd_enabled(config)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = sd_read_blob(config, key, dst, len_inout);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND &&
        ret != ESP_ERR_NOT_FINISHED) {
        note_sd_failure(config, D1L_RETAINED_SD_OP_READ, ret);
    }
    return ret;
}

esp_err_t d1l_retained_blob_store_read_nvs_fallback(
    d1l_retained_blob_store_id_t store_id,
    const char *key,
    void *dst,
    size_t *len_inout)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !key || !dst || !len_inout) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_read_blob(config, key, dst, len_inout);
}

esp_err_t d1l_retained_blob_store_write_sd_primary(
    d1l_retained_blob_store_id_t store_id,
    const char *key,
    const void *src,
    size_t len)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !key || !src || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_sd_enabled(config)) {
        return ESP_ERR_INVALID_STATE;
    }
    return sd_write_blob(config, key, src, len);
}

esp_err_t d1l_retained_blob_store_write_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id,
    const char *key,
    const void *src,
    size_t len,
    uint32_t expected_generation)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !key || !src || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return sd_write_blob_with_lineage(config, key, src, len,
                                      expected_generation);
}

esp_err_t d1l_retained_blob_store_write_nvs_fallback(
    d1l_retained_blob_store_id_t store_id,
    const char *key,
    const void *src,
    size_t len)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !key || !src || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = nvs_write_blob(config, key, src, len);
    note_nvs_mirror_failure(config, ret);
    return ret;
}

esp_err_t d1l_retained_blob_store_erase_sd_primary(
    d1l_retained_blob_store_id_t store_id,
    const char *key)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !key) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_sd_enabled(config)) {
        return ESP_ERR_INVALID_STATE;
    }
    return sd_erase_blob(config, key);
}

esp_err_t d1l_retained_blob_store_erase_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id,
    const char *key,
    uint32_t expected_generation)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !key) {
        return ESP_ERR_INVALID_ARG;
    }
    return sd_erase_blob_with_lineage(config, key, expected_generation);
}

esp_err_t d1l_retained_blob_store_sd_media_lineage_ready(
    d1l_retained_blob_store_id_t store_id,
    uint32_t expected_backend_generation, bool *out_ready)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !out_ready) {
        return ESP_ERR_INVALID_ARG;
    }
    bool active = true;
    bool marker_matches = false;
    uint32_t reset_generation = 0U;
    return sd_media_lineage_ready(
        config, expected_backend_generation, out_ready, &active,
        &reset_generation, &marker_matches);
}

esp_err_t d1l_retained_blob_store_complete_packet_reset_lineage(
    uint32_t expected_backend_generation)
{
    const d1l_retained_blob_store_config_t *config = find_store(
        D1L_RETAINED_BLOB_STORE_PACKET_LOG);
    if (!config || !store_backend_generation_matches(
                       config, expected_backend_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    bool ready = false;
    bool active = true;
    bool marker_matches = false;
    uint32_t lineage_generation = 0U;
    esp_err_t ret = sd_media_lineage_ready(
        config, expected_backend_generation, &ready, &active,
        &lineage_generation, &marker_matches);
    (void)active;
    (void)marker_matches;
    if (ret != ESP_OK || ready) {
        return ret;
    }
    ret = sd_purge_store_for_generation(config,
                                        expected_backend_generation);
    if (ret != ESP_OK) {
        return ret;
    }
    return sd_finalize_lineage_for_generation(
        config, lineage_generation, expected_backend_generation);
}

esp_err_t d1l_retained_blob_store_erase_nvs_fallback(
    d1l_retained_blob_store_id_t store_id,
    const char *key)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !key) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_erase_blob(config, key);
}

esp_err_t d1l_retained_blob_store_read(d1l_retained_blob_store_id_t store_id,
                                       const char *key,
                                       void *dst,
                                       size_t *len_inout)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !key || !dst || !len_inout) {
        return ESP_ERR_INVALID_ARG;
    }

    if (store_sd_enabled(config)) {
        const size_t requested_len = *len_inout;
        esp_err_t sd_ret = sd_read_blob(config, key, dst, len_inout);
        if (sd_ret == ESP_OK) {
            return ESP_OK;
        }
        if (sd_ret == ESP_ERR_NOT_FINISHED) {
            return sd_ret;
        }
        note_sd_failure(config, D1L_RETAINED_SD_OP_READ, sd_ret);
        *len_inout = requested_len;
        esp_err_t nvs_ret = nvs_read_blob(config, key, dst, len_inout);
        if (nvs_ret == ESP_OK) {
            return ESP_OK;
        }
        return sd_ret == ESP_ERR_NOT_FOUND ? nvs_ret : sd_ret;
    }

    return nvs_read_blob(config, key, dst, len_inout);
}

esp_err_t d1l_retained_blob_store_read_fallback(d1l_retained_blob_store_id_t store_id,
                                                const char *key,
                                                void *dst,
                                                size_t *len_inout)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !key || !dst || !len_inout) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_read_blob(config, key, dst, len_inout);
}

esp_err_t d1l_retained_blob_store_write(d1l_retained_blob_store_id_t store_id,
                                        const char *key,
                                        const void *src,
                                        size_t len)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !key || !src || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (store_sd_enabled(config)) {
        esp_err_t sd_ret = sd_write_blob(config, key, src, len);
        if (sd_ret == ESP_OK) {
            note_nvs_mirror_failure(config, nvs_write_blob(config, key, src, len));
            return ESP_OK;
        }
    }

    return nvs_write_blob(config, key, src, len);
}

esp_err_t d1l_retained_blob_store_write_split(d1l_retained_blob_store_id_t store_id,
                                              const char *key,
                                              const void *primary_src,
                                              size_t primary_len,
                                              const void *fallback_src,
                                              size_t fallback_len)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    const bool has_primary = primary_src && primary_len > 0;
    const bool has_fallback = fallback_src && fallback_len > 0;
    if (!config || !key || (!has_primary && !has_fallback)) {
        return ESP_ERR_INVALID_ARG;
    }

    const void *nvs_src = has_fallback ? fallback_src : primary_src;
    const size_t nvs_len = has_fallback ? fallback_len : primary_len;
    if (!store_sd_enabled(config) || !has_primary) {
        return nvs_write_blob(config, key, nvs_src, nvs_len);
    }

    esp_err_t sd_ret = sd_write_blob(config, key, primary_src, primary_len);
    esp_err_t nvs_ret = nvs_write_blob(config, key, nvs_src, nvs_len);
    if (sd_ret == ESP_OK) {
        note_nvs_mirror_failure(config, nvs_ret);
        return ESP_OK;
    }
    return nvs_ret == ESP_OK ? ESP_OK : sd_ret;
}

esp_err_t d1l_retained_blob_store_erase(d1l_retained_blob_store_id_t store_id,
                                        const char *key)
{
    const d1l_retained_blob_store_config_t *config = find_store(store_id);
    if (!config || !key) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    if (store_sd_enabled(config)) {
        ret = sd_erase_blob(config, key);
        if (ret == ESP_ERR_NOT_FOUND) {
            ret = ESP_OK;
        }
    }
    esp_err_t nvs_ret = nvs_erase_blob(config, key);
    return ret == ESP_OK ? nvs_ret : ret;
}
