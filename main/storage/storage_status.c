#include "storage_status.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hal/rp2040_bridge.h"
#include "mesh/route_store_worker.h"
#include "storage/export_store.h"
#include "storage/map_tile_store.h"
#include "storage/retained_blob_store.h"
#include "storage/storage_status_policy.h"

#define D1L_STORAGE_BOOT_POLL_INTERVAL_MS 250U
#define D1L_STORAGE_BOOT_POLL_TIMEOUT_MS 500U
#define D1L_STORAGE_BOOT_POLL_ATTEMPTS 40U
#define D1L_STORAGE_MANAGER_STACK_BYTES 4096U
#define D1L_STORAGE_MANAGER_IDLE_INTERVAL_MS 2000U
#define D1L_STORAGE_MANAGER_BACKOFF_MS 5000U
#define D1L_STORAGE_MANAGER_RESET_HOLD_MS 500U
#define D1L_STORAGE_MANAGER_RESET_SETTLE_MS 8000U
#define D1L_STORAGE_MANAGER_RECOVERY_PING_ATTEMPTS 8U
#define D1L_STORAGE_MANAGER_RECOVERY_PING_INTERVAL_MS 500U
#define D1L_STORAGE_MANAGER_INITIAL_PING_TIMEOUT_LIMIT 3U
#define D1L_STORAGE_MANAGER_PASSIVE_QUIESCE_TIMEOUT_MS 250U

typedef enum {
    D1L_STORAGE_MANAGER_BRIDGE_WAIT,
    D1L_STORAGE_MANAGER_PING,
    D1L_STORAGE_MANAGER_STATUS,
    D1L_STORAGE_MANAGER_MOUNT,
    D1L_STORAGE_MANAGER_READY_SD,
    D1L_STORAGE_MANAGER_READY_VOLATILE,
    D1L_STORAGE_MANAGER_NEEDS_FAT32,
    D1L_STORAGE_MANAGER_NO_CARD,
    D1L_STORAGE_MANAGER_ERROR_BACKOFF,
} d1l_storage_manager_state_t;

static d1l_storage_status_t s_status;
static TaskHandle_t s_storage_manager_task;
static d1l_storage_manager_state_t s_manager_state = D1L_STORAGE_MANAGER_BRIDGE_WAIT;
static bool s_manager_remount_requested;
static bool s_manager_reset_bridge_requested;
static int64_t s_manager_pause_until_us;
static portMUX_TYPE s_manager_pause_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_manager_control_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_manager_sequence_mutex_storage;
static SemaphoreHandle_t s_manager_sequence_mutex;
static TaskHandle_t s_manager_quiesce_owner;
static portMUX_TYPE s_manager_quiesce_owner_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_force_nvs;
static uint32_t s_manager_initial_ping_timeouts;
static bool s_manager_bridge_response_seen;
static bool s_manager_auto_reset_pending;
static bool s_manager_auto_reset_attempted;
static bool s_manager_initial_reset_budget_spent;
static bool s_manager_reset_recovery_active;

static esp_err_t storage_status_mount(uint32_t timeout_ms, bool force_bridge_mount);

static void refresh_retained_sd_health(d1l_storage_status_t *status)
{
    if (!status) {
        return;
    }
    (void)d1l_retained_blob_store_sd_stats(D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES,
                                           &status->retained_sd_stats[
                                               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES]);
    (void)d1l_retained_blob_store_sd_stats(D1L_RETAINED_BLOB_STORE_DM_MESSAGES,
                                           &status->retained_sd_stats[
                                               D1L_RETAINED_BLOB_STORE_DM_MESSAGES]);
    (void)d1l_retained_blob_store_sd_stats(D1L_RETAINED_BLOB_STORE_ROUTES,
                                           &status->retained_sd_stats[
                                               D1L_RETAINED_BLOB_STORE_ROUTES]);
    (void)d1l_retained_blob_store_sd_stats(D1L_RETAINED_BLOB_STORE_PACKET_LOG,
                                           &status->retained_sd_stats[
                                               D1L_RETAINED_BLOB_STORE_PACKET_LOG]);
    (void)d1l_retained_blob_store_sd_stats(D1L_RETAINED_BLOB_STORE_NODES,
                                           &status->retained_sd_stats[
                                               D1L_RETAINED_BLOB_STORE_NODES]);
    (void)d1l_retained_blob_store_sd_stats(D1L_RETAINED_BLOB_STORE_CONTACTS,
                                           &status->retained_sd_stats[
                                               D1L_RETAINED_BLOB_STORE_CONTACTS]);
    (void)d1l_retained_blob_store_sd_stats(D1L_RETAINED_BLOB_STORE_READ_STATE,
                                           &status->retained_sd_stats[
                                               D1L_RETAINED_BLOB_STORE_READ_STATE]);
    status->retained_sd_degraded = d1l_retained_blob_store_any_sd_degraded();
    /* RC1 retained history is SD-only. Internal NVS is not a history mirror,
     * so its ownership/legacy-import state must not be presented as a failed
     * backup. */
    status->retained_backup_degraded = false;
    if (status->retained_sd_degraded) {
        status->note = D1L_RETAINED_BLOB_STORE_SD_DEGRADED_NOTE;
    }
}

static const char *storage_manager_state_name(d1l_storage_manager_state_t state)
{
    switch (state) {
    case D1L_STORAGE_MANAGER_BRIDGE_WAIT:
        return "BRIDGE_WAIT";
    case D1L_STORAGE_MANAGER_PING:
        return "PING";
    case D1L_STORAGE_MANAGER_STATUS:
        return "STATUS";
    case D1L_STORAGE_MANAGER_MOUNT:
        return "MOUNT";
    case D1L_STORAGE_MANAGER_READY_SD:
        return "READY_SD";
    case D1L_STORAGE_MANAGER_READY_VOLATILE:
        return "READY_VOLATILE";
    case D1L_STORAGE_MANAGER_NEEDS_FAT32:
        return "NEEDS_FAT32";
    case D1L_STORAGE_MANAGER_NO_CARD:
        return "NO_CARD";
    case D1L_STORAGE_MANAGER_ERROR_BACKOFF:
        return "ERROR_BACKOFF";
    default:
        return "ERROR_BACKOFF";
    }
}

static void set_manager_state(d1l_storage_manager_state_t state)
{
    s_manager_state = state;
    s_status.manager_state = storage_manager_state_name(state);
}

static void ensure_manager_sequence_mutex(void)
{
    if (!s_manager_sequence_mutex) {
        s_manager_sequence_mutex =
            xSemaphoreCreateMutexStatic(&s_manager_sequence_mutex_storage);
    }
}

static esp_err_t manager_sequence_take(uint32_t timeout_ms)
{
    ensure_manager_sequence_mutex();
    if (!s_manager_sequence_mutex) {
        return ESP_ERR_NO_MEM;
    }
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0U && ticks == 0U) {
        ticks = 1U;
    }
    return xSemaphoreTake(s_manager_sequence_mutex, ticks) == pdTRUE ?
        ESP_OK : ESP_ERR_TIMEOUT;
}

static bool manager_sequence_try_take(void)
{
    return manager_sequence_take(0U) == ESP_OK;
}

static void manager_sequence_give(void)
{
    if (s_manager_sequence_mutex) {
        xSemaphoreGive(s_manager_sequence_mutex);
    }
}

static void sync_initial_recovery_status_locked(void)
{
    s_status.manager_initial_ping_timeouts = s_manager_initial_ping_timeouts;
    s_status.manager_bridge_response_seen = s_manager_bridge_response_seen;
    s_status.manager_auto_reset_pending = s_manager_auto_reset_pending;
    s_status.manager_auto_reset_attempted = s_manager_auto_reset_attempted;
}

void d1l_storage_status_note_valid_bridge_response(void)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    s_manager_bridge_response_seen = true;
    s_manager_initial_ping_timeouts = 0U;
    s_manager_auto_reset_pending = false;
    sync_initial_recovery_status_locked();
    portEXIT_CRITICAL(&s_manager_control_lock);
}

static bool initial_ping_timeout_requires_reset(esp_err_t result)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    if (s_manager_bridge_response_seen || s_manager_auto_reset_attempted ||
        s_manager_initial_reset_budget_spent) {
        sync_initial_recovery_status_locked();
        portEXIT_CRITICAL(&s_manager_control_lock);
        return false;
    }
    if (result != ESP_ERR_TIMEOUT) {
        s_manager_initial_ping_timeouts = 0U;
        sync_initial_recovery_status_locked();
        portEXIT_CRITICAL(&s_manager_control_lock);
        return false;
    }
    if (s_manager_initial_ping_timeouts <
        D1L_STORAGE_MANAGER_INITIAL_PING_TIMEOUT_LIMIT) {
        ++s_manager_initial_ping_timeouts;
    }
    if (s_manager_initial_ping_timeouts <
        D1L_STORAGE_MANAGER_INITIAL_PING_TIMEOUT_LIMIT) {
        sync_initial_recovery_status_locked();
        portEXIT_CRITICAL(&s_manager_control_lock);
        return false;
    }
    s_manager_auto_reset_pending = true;
    sync_initial_recovery_status_locked();
    portEXIT_CRITICAL(&s_manager_control_lock);
    return true;
}

static bool claim_initial_bridge_reset(void)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    const bool claimed = s_manager_auto_reset_pending &&
                         !s_manager_bridge_response_seen &&
                         !s_manager_auto_reset_attempted &&
                         !s_manager_initial_reset_budget_spent &&
                         !s_manager_reset_recovery_active &&
                         !s_force_nvs;
    s_manager_auto_reset_pending = false;
    if (claimed) {
        s_manager_auto_reset_attempted = true;
        s_manager_initial_reset_budget_spent = true;
        s_manager_initial_ping_timeouts = 0U;
        s_manager_reset_recovery_active = true;
    }
    sync_initial_recovery_status_locked();
    portEXIT_CRITICAL(&s_manager_control_lock);
    return claimed;
}

static bool initial_bridge_reset_is_pending(void)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    const bool pending = s_manager_auto_reset_pending &&
                         !s_manager_bridge_response_seen &&
                         !s_manager_auto_reset_attempted &&
                         !s_force_nvs;
    portEXIT_CRITICAL(&s_manager_control_lock);
    return pending;
}

static void manager_control_take(bool *out_force_nvs,
                                 bool *out_reset_bridge,
                                 bool *out_remount)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    if (out_force_nvs) {
        *out_force_nvs = s_force_nvs;
    }
    if (out_reset_bridge) {
        *out_reset_bridge = s_manager_reset_bridge_requested;
    }
    if (out_remount) {
        *out_remount = s_manager_remount_requested;
    }
    s_manager_reset_bridge_requested = false;
    s_manager_remount_requested = false;
    portEXIT_CRITICAL(&s_manager_control_lock);
}

static bool manager_force_nvs_enabled(void)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    const bool force_nvs = s_force_nvs;
    portEXIT_CRITICAL(&s_manager_control_lock);
    return force_nvs;
}

static bool manager_control_begin_manual_reset(void)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    const bool claimed = !s_force_nvs && !s_manager_reset_recovery_active;
    if (claimed) {
        s_manager_initial_ping_timeouts = 0U;
        s_manager_auto_reset_pending = false;
        s_manager_initial_reset_budget_spent = true;
        s_manager_reset_recovery_active = true;
        s_manager_reset_bridge_requested = false;
        s_manager_remount_requested = false;
        sync_initial_recovery_status_locked();
    }
    portEXIT_CRITICAL(&s_manager_control_lock);
    return claimed;
}

static void manager_control_satisfy_reset(void)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    /* A request received while a reset pulse was already in progress is
     * coalesced into that pulse and its following mount attempt. */
    s_manager_initial_ping_timeouts = 0U;
    s_manager_auto_reset_pending = false;
    s_manager_initial_reset_budget_spent = true;
    s_manager_reset_bridge_requested = false;
    s_manager_remount_requested = false;
    sync_initial_recovery_status_locked();
    portEXIT_CRITICAL(&s_manager_control_lock);
}

static void manager_control_finish_reset_recovery(void)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    s_manager_reset_recovery_active = false;
    portEXIT_CRITICAL(&s_manager_control_lock);
}

static void set_store_backends(d1l_storage_status_t *status)
{
    const bool public_messages_on_sd =
        d1l_retained_blob_store_uses_sd(D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES);
    const bool dm_messages_on_sd =
        d1l_retained_blob_store_uses_sd(D1L_RETAINED_BLOB_STORE_DM_MESSAGES);
    const bool routes_on_sd =
        d1l_retained_blob_store_uses_sd(D1L_RETAINED_BLOB_STORE_ROUTES);
    const bool packet_log_on_sd =
        d1l_retained_blob_store_uses_sd(D1L_RETAINED_BLOB_STORE_PACKET_LOG);
    const bool nodes_on_sd =
        d1l_retained_blob_store_uses_sd(D1L_RETAINED_BLOB_STORE_NODES);
    const bool contacts_on_sd =
        d1l_retained_blob_store_uses_sd(D1L_RETAINED_BLOB_STORE_CONTACTS);
    const bool read_state_on_sd =
        d1l_retained_blob_store_uses_sd(D1L_RETAINED_BLOB_STORE_READ_STATE);
    const bool any_retained_sd = public_messages_on_sd ||
                                 dm_messages_on_sd ||
                                 routes_on_sd ||
                                 packet_log_on_sd ||
                                 nodes_on_sd ||
                                 contacts_on_sd ||
                                 read_state_on_sd;
    status->data_backend = any_retained_sd ? "sd" : "volatile";
    status->message_store_backend =
        d1l_retained_blob_store_backend_name(D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES);
    status->dm_store_backend =
        d1l_retained_blob_store_backend_name(D1L_RETAINED_BLOB_STORE_DM_MESSAGES);
    status->packet_log_backend =
        d1l_retained_blob_store_backend_name(D1L_RETAINED_BLOB_STORE_PACKET_LOG);
    status->route_store_backend =
        d1l_retained_blob_store_backend_name(D1L_RETAINED_BLOB_STORE_ROUTES);
    status->node_store_backend =
        d1l_retained_blob_store_backend_name(D1L_RETAINED_BLOB_STORE_NODES);
    status->contact_store_backend =
        d1l_retained_blob_store_backend_name(D1L_RETAINED_BLOB_STORE_CONTACTS);
    status->read_state_backend =
        d1l_retained_blob_store_backend_name(D1L_RETAINED_BLOB_STORE_READ_STATE);
    status->map_tile_backend = d1l_map_tile_store_sd_ready(status) ?
        "sd_map_tiles_ready" : "unavailable";
    status->export_backend = d1l_export_store_sd_ready(status) ?
        "sd_diagnostic_exports_ready" : "serial";
    status->data_enabled = any_retained_sd;
    refresh_retained_sd_health(status);
}

static void pause_retained_sd_backend_for_manager_failure(void)
{
    d1l_retained_blob_store_note_sd_backend(false, false, false, 0U, 0U, 0U);
    set_store_backends(&s_status);
}

static void set_default_actions(d1l_storage_status_t *status)
{
    status->setup_action = "not_available";
    status->sd_filesystem = "unknown";
    status->file_ops_supported = false;
    status->atomic_rename_supported = false;
    status->file_line_max = 0;
    status->file_chunk_max = 0;
    status->path_max = 0;
}

static bool storage_sd_ready_for_files(void)
{
    return d1l_storage_status_policy_allows_cached_io(
               s_status.bridge_status_refresh_failures) &&
           s_status.rp2040_sd_protocol_supported &&
           s_status.sd_present &&
           s_status.sd_mounted &&
           s_status.sd_data_root_ready &&
           !s_status.sd_needs_fat32 &&
           s_status.file_ops_supported &&
           s_status.atomic_rename_supported;
}

static void apply_force_nvs_status(void)
{
    d1l_retained_blob_store_note_sd_backend(false, false, false, 0, 0, 0);
    set_store_backends(&s_status);
    s_status.force_nvs = true;
    s_status.data_enabled = false;
    s_status.setup_action = "forced_volatile";
    s_status.note = "SD history writes are paused; live RF and chat continue without saved history until storage force-nvs off or storage remount is requested";
    set_manager_state(D1L_STORAGE_MANAGER_READY_VOLATILE);
}

static void clear_sd_runtime_fields(d1l_storage_status_t *status)
{
    status->rp2040_sd_protocol_supported = false;
    status->sd_present = false;
    status->sd_presence_stale = false;
    status->sd_mounted = false;
    status->sd_data_root_ready = false;
    status->sd_needs_fat32 = false;
    status->setup_required = false;
    status->setup_supported = false;
    status->file_ops_supported = false;
    status->atomic_rename_supported = false;
    status->response_truncated = false;
    status->capacity_kb = 0;
    status->free_kb = 0;
    status->file_line_max = 0;
    status->file_chunk_max = 0;
    status->path_max = 0;
    status->sd_probe_error = 0;
    status->sd_probe_data = 0;
    status->sd_mount_error = 0;
    status->sd_mount_data = 0;
    status->sd_filesystem = "unknown";
    status->sd_probe_power[0] = '\0';
    status->sd_probe_mode[0] = '\0';
}

static const char *stable_sd_state(const char *state)
{
    if (!state || state[0] == '\0') {
        return "unknown";
    }
    if (strcmp(state, "ready") == 0) {
        return "ready";
    }
    if (strcmp(state, "no_card") == 0) {
        return "no_card";
    }
    if (strcmp(state, "setup_required") == 0) {
        return "not_fat32_or_unmountable";
    }
    if (strcmp(state, "unformatted") == 0) {
        return "not_fat32_or_unmountable";
    }
    if (strcmp(state, "not_fat32_or_unmountable") == 0) {
        return "not_fat32_or_unmountable";
    }
    if (strcmp(state, "deskos_manifest_invalid") == 0) {
        return "deskos_manifest_invalid";
    }
    if (strcmp(state, "creating_deskos_files") == 0) {
        return "creating_deskos_files";
    }
    if (strcmp(state, "fat32_ready") == 0) {
        return "fat32_ready";
    }
    if (strcmp(state, "checking") == 0) {
        return "checking";
    }
    if (strcmp(state, "error") == 0) {
        return "error";
    }
    if (strcmp(state, "mount_required") == 0) {
        return "mount_required";
    }
    if (strcmp(state, "mount_pending") == 0) {
        return "mount_pending";
    }
    if (strcmp(state, "protocol_pending") == 0) {
        return "protocol_pending";
    }
    if (strcmp(state, "bridge_unavailable") == 0) {
        return "bridge_unavailable";
    }
    return "bridge_reported";
}

static const char *stable_filesystem(const char *filesystem)
{
    if (!filesystem || filesystem[0] == '\0') {
        return "unknown";
    }
    if (strcmp(filesystem, "fat32") == 0) {
        return "fat32";
    }
    if (strcmp(filesystem, "exfat") == 0) {
        return "exfat";
    }
    if (strcmp(filesystem, "fatfs") == 0) {
        return "fatfs";
    }
    return "reported";
}

static void apply_rp2040_sd_status(const d1l_rp2040_sd_status_t *sd)
{
    const bool previous_present = s_status.sd_present;
    const uint32_t previous_capacity_kb = s_status.capacity_kb;
    const uint32_t previous_free_kb = s_status.free_kb;
    const char *previous_filesystem = s_status.sd_filesystem;
    const bool explicit_no_card = strcmp(sd->state, "no_card") == 0;
    const bool effective_present =
        d1l_storage_status_policy_effective_present(previous_present,
                                                    sd->state,
                                                    sd->card_present);
    const bool presence_stale = previous_present && !sd->card_present &&
                                !explicit_no_card;
    const bool mount_failed_with_diag = effective_present &&
                                         !sd->filesystem_mounted &&
                                         (sd->mount_error != 0U || sd->mount_data != 0U);
    const bool probe_rejected_card =
        strcmp(sd->note, "sd_probe_rejected_card") == 0 ||
        ((strcmp(sd->state, "error") == 0 || !sd->card_present) &&
         (sd->probe_error == 3U || sd->probe_error == 4U));

    if (sd->bridge_ready && sd->protocol_supported) {
        d1l_storage_status_note_valid_bridge_response();
    }

    s_status.rp2040_bridge_ready = sd->bridge_ready;
    s_status.rp2040_sd_protocol_supported = sd->protocol_supported;
    s_status.sd_present = effective_present;
    s_status.sd_presence_stale = presence_stale;
    s_status.sd_mounted = sd->filesystem_mounted;
    s_status.sd_data_root_ready = sd->deskos_root_ready;
    s_status.sd_needs_fat32 = !probe_rejected_card &&
                               (sd->needs_fat32 ||
                                (effective_present && !sd->filesystem_mounted &&
                                 !mount_failed_with_diag && !presence_stale));
    s_status.setup_required = effective_present && (!sd->filesystem_mounted ||
                                                     !sd->deskos_root_ready ||
                                                     s_status.sd_needs_fat32);
    s_status.setup_supported = sd->protocol_supported && effective_present;
    s_status.file_ops_supported = sd->file_ops_supported;
    s_status.atomic_rename_supported = sd->atomic_rename_supported;
    s_status.response_truncated = sd->response_truncated;
    s_status.capacity_kb = presence_stale ? previous_capacity_kb : sd->capacity_kb;
    s_status.free_kb = presence_stale ? previous_free_kb : sd->free_kb;
    s_status.file_line_max = sd->file_line_max;
    s_status.file_chunk_max = sd->file_chunk_max;
    s_status.path_max = sd->path_max;
    s_status.sd_probe_error = sd->probe_error;
    s_status.sd_probe_data = sd->probe_data;
    s_status.sd_mount_error = sd->mount_error;
    s_status.sd_mount_data = sd->mount_data;
    snprintf(s_status.sd_probe_power, sizeof(s_status.sd_probe_power), "%s",
             sd->probe_power[0] ? sd->probe_power : "unknown");
    snprintf(s_status.sd_probe_mode, sizeof(s_status.sd_probe_mode), "%s",
             sd->probe_mode[0] ? sd->probe_mode : "unknown");
    s_status.last_error = sd->last_error;
    s_status.bridge_status_refresh_failures = 0U;
    s_status.bridge_status_stale = false;
    s_status.sd_state = stable_sd_state(sd->state);
    s_status.sd_filesystem = presence_stale ? previous_filesystem :
                             stable_filesystem(sd->filesystem);
    d1l_retained_blob_store_note_sd_backend(sd->data_ready,
                                            sd->file_ops_supported,
                                            sd->atomic_rename_supported,
                                            sd->file_line_max,
                                            sd->file_chunk_max,
                                            sd->path_max);
    set_store_backends(&s_status);

    if (!sd->bridge_ready) {
        s_status.setup_action = "bridge_unavailable";
        s_status.note = "RP2040 UART bridge is unavailable; live RF and chat continue but history is not saved";
    } else if (!sd->protocol_supported) {
        s_status.setup_action = "bridge_protocol_pending";
        s_status.note = "RP2040 UART is ready, but the DeskOS SD status protocol is not implemented on the bridge yet";
    } else if (strcmp(s_status.sd_state, "mount_required") == 0) {
        s_status.setup_action = "run_storage_mount";
        s_status.note = "RP2040 SD bridge is ready; run storage mount to check the inserted card before enabling SD data storage";
    } else if (strcmp(s_status.sd_state, "mount_pending") == 0) {
        s_status.setup_action = "wait_for_storage_mount";
        s_status.note = "RP2040 SD bridge is checking the inserted card; history is live-only until the mount completes";
    } else if (probe_rejected_card) {
        s_status.setup_action = "inspect_rp2040_sd_cmd0_firmware_path";
        s_status.note =
            "RP2040 SD probe rejected the card init response; inspect firmware CMD0/CMD8 diagnostics before changing the card";
    } else if (explicit_no_card) {
        s_status.setup_action = "insert_card";
        s_status.note = "No SD card reported by the RP2040 bridge; live RF and chat continue but history is not saved";
    } else if (mount_failed_with_diag) {
        s_status.setup_action = "inspect_rp2040_sd_mount_error_firmware_path";
        s_status.note =
            "The last confirmed card is not mounted; inspect RP2040 mount diagnostics while history remains live-only";
    } else if (presence_stale) {
        s_status.setup_action = "wait_for_storage_reconnect";
        s_status.note =
            "The card was previously confirmed, but the latest bridge reply could not confirm its presence; history is live-only";
    } else if (s_status.sd_needs_fat32) {
        s_status.setup_action = "prepare_fat32_on_computer";
        s_status.note =
            "SD card is present but not usable; prepare a FAT32 card on a computer and reinsert it";
    } else if (!sd->deskos_root_ready) {
        s_status.setup_action =
            strcmp(s_status.sd_state, "deskos_manifest_invalid") == 0 ?
            "backup_reformat_fat32_on_computer" : "retry_storage_mount";
        s_status.note =
            strcmp(s_status.sd_state, "deskos_manifest_invalid") == 0 ?
            "DeskOS files are invalid; back up the card and prepare FAT32 on a computer" :
            "DeskOS files are not ready; retry storage mount to create the required folders";
    } else if (s_status.setup_required) {
        s_status.setup_action = "live_only_until_ready";
        s_status.note = "SD card is present but not ready; history is live-only until storage is ready";
    } else {
        s_status.setup_action = s_status.data_enabled ? "retained_history_sd_enabled" :
                                "store_migration_pending";
        s_status.note = s_status.data_enabled ?
            "SD card is valid; Public/DM message, route, packet, node history, diagnostic exports, and map tiles use SD" :
            "SD card is valid, but saved-history stores are still starting";
        s_status.map_tile_backend = d1l_map_tile_store_sd_ready(&s_status) ?
            "sd_map_tiles_ready" : "sd_pending_store_migration";
    }
}

static void invalidate_stale_bridge_operational_state(void)
{
    /* Preserve last-confirmed capacity, filesystem, and probe diagnostics,
     * but never expose cached protocol or file capabilities after the bounded
     * reconnect grace expires. Presence remains a last-confirmed diagnostic;
     * it is explicitly marked stale instead of being reported as removal. */
    s_status.rp2040_sd_protocol_supported = false;
    s_status.sd_presence_stale = s_status.sd_present;
    s_status.sd_mounted = false;
    s_status.sd_data_root_ready = false;
    s_status.sd_needs_fat32 = false;
    s_status.setup_required = false;
    s_status.setup_supported = false;
    s_status.file_ops_supported = false;
    s_status.atomic_rename_supported = false;
    s_status.file_line_max = 0U;
    s_status.file_chunk_max = 0U;
    s_status.path_max = 0U;
    s_status.sd_state = s_status.rp2040_bridge_ready ?
                        "protocol_pending" : "rp2040_unavailable";
}

static void note_rp2040_exchange_failure(esp_err_t ret, bool response_truncated)
{
    s_status.bridge_status_refresh_failures =
        d1l_storage_status_policy_note_failure(
            s_status.bridge_status_refresh_failures);
    s_status.bridge_status_stale =
        d1l_storage_status_policy_is_stale(
            s_status.bridge_status_refresh_failures);
    const bool allows_cached_io =
        d1l_storage_status_policy_allows_cached_io(
            s_status.bridge_status_refresh_failures);
    if (!allows_cached_io) {
        d1l_retained_blob_store_note_sd_backend(false, false, false, 0, 0, 0);
        set_store_backends(&s_status);
        invalidate_stale_bridge_operational_state();
    }
    s_status.response_truncated = response_truncated;
    s_status.last_error = ret;
    s_status.setup_action = "wait_for_storage_reconnect";
    s_status.note = allows_cached_io ?
        "Storage status is reconnecting; the last confirmed SD state remains active during a bounded grace period" :
        "Storage status did not recover; SD file access is paused and history is live-only until a valid status reply";
}

static void apply_rp2040_sd_exchange_result(const d1l_rp2040_sd_status_t *sd,
                                            esp_err_t ret)
{
    if (ret == ESP_OK) {
        apply_rp2040_sd_status(sd);
        return;
    }

    /* A timeout or malformed reply is not evidence that an inserted card
     * disappeared. Keep the last parsed card/filesystem diagnostics, allow a
     * short I/O grace, then fail closed to live-only history. A real removal is
     * still applied immediately because it arrives as a valid no_card reply. */
    note_rp2040_exchange_failure(ret, sd->response_truncated);
}

esp_err_t d1l_storage_status_init(void)
{
    ensure_manager_sequence_mutex();
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.mount_point = D1L_STORAGE_SD_MOUNT_POINT;
    s_status.data_root = D1L_STORAGE_SD_DATA_ROOT;
    s_status.sd_filesystem = "unknown";
    s_status.manager_state = storage_manager_state_name(s_manager_state);
    s_status.manager_running = s_storage_manager_task != NULL;
    s_status.manager_attempt = 0;
    s_status.manager_backoff_ms = 0;
    portENTER_CRITICAL(&s_manager_control_lock);
    s_manager_initial_ping_timeouts = 0U;
    s_manager_bridge_response_seen = false;
    s_manager_auto_reset_pending = false;
    s_manager_auto_reset_attempted = false;
    s_manager_initial_reset_budget_spent = false;
    s_manager_reset_recovery_active = false;
    s_status.force_nvs = s_force_nvs;
    sync_initial_recovery_status_locked();
    portEXIT_CRITICAL(&s_manager_control_lock);
    s_status.last_error = ESP_ERR_NOT_SUPPORTED;

#if CONFIG_LCD_BOARD_SENSECAP_INDICATOR_D1L
    s_status.direct_supported = false;
    s_status.rp2040_bridge_required = true;
    s_status.sd_interface = "rp2040";
    s_status.sd_state = "pending_bridge";
    s_status.note =
        "D1L microSD uses the RP2040 bridge; history remains live-only until that bridge is ready";
#else
    s_status.direct_supported = false;
    s_status.rp2040_bridge_required = false;
    s_status.sd_interface = "unknown";
    s_status.sd_state = "unsupported";
    s_status.note = "SD data storage is not enabled for this board profile";
#endif

    d1l_retained_blob_store_note_sd_backend(false, false, false, 0, 0, 0);
    set_store_backends(&s_status);
    set_default_actions(&s_status);
    return ESP_OK;
}

void d1l_storage_status_note_rp2040(esp_err_t rp2040_init_result)
{
    if (!s_status.initialized) {
        (void)d1l_storage_status_init();
    }
    if (manager_force_nvs_enabled()) {
        apply_force_nvs_status();
        return;
    }
    s_status.rp2040_bridge_ready = (rp2040_init_result == ESP_OK);
    if (s_status.rp2040_bridge_required && !s_status.rp2040_bridge_ready) {
        clear_sd_runtime_fields(&s_status);
        d1l_retained_blob_store_note_sd_backend(false, false, false, 0, 0, 0);
        set_store_backends(&s_status);
        s_status.sd_state = "rp2040_unavailable";
        s_status.last_error = rp2040_init_result;
        s_status.setup_action = "bridge_unavailable";
        s_status.note = "RP2040 bridge is not ready; live RF and chat continue but history is not saved";
    } else if (s_status.rp2040_bridge_required) {
        clear_sd_runtime_fields(&s_status);
        d1l_retained_blob_store_note_sd_backend(false, false, false, 0, 0, 0);
        set_store_backends(&s_status);
        s_status.sd_state = "protocol_pending";
        s_status.last_error = ESP_ERR_NOT_SUPPORTED;
        s_status.setup_action = "bridge_protocol_pending";
        s_status.note = "RP2040 UART is ready; DeskOS SD status protocol is pending on the bridge";
    }
}

esp_err_t d1l_storage_status_refresh(uint32_t timeout_ms)
{
    if (!s_status.initialized) {
        (void)d1l_storage_status_init();
    }

    if (!s_status.rp2040_bridge_required) {
        s_status.last_error = ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_NOT_SUPPORTED;
    }

    d1l_rp2040_sd_status_t sd = {0};
    esp_err_t ret = d1l_rp2040_bridge_probe_sd(&sd, timeout_ms);
    apply_rp2040_sd_exchange_result(&sd, ret);
    if (manager_force_nvs_enabled()) {
        apply_force_nvs_status();
    }
    return ret;
}

static uint32_t storage_deadline_remaining_ms(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0U;
    }
    return (uint32_t)((remaining_us + 999LL) / 1000LL);
}

static esp_err_t poll_mount_pending_until(int64_t deadline_us)
{
    esp_err_t last_ret = ESP_OK;
    for (uint32_t attempt = 0; attempt < D1L_STORAGE_BOOT_POLL_ATTEMPTS; ++attempt) {
        if (strcmp(s_status.sd_state, "mount_pending") != 0) {
            break;
        }
        uint32_t remaining_ms = storage_deadline_remaining_ms(deadline_us);
        if (remaining_ms == 0U) {
            return ESP_ERR_TIMEOUT;
        }
        const uint32_t delay_ms = remaining_ms < D1L_STORAGE_BOOT_POLL_INTERVAL_MS ?
            remaining_ms : D1L_STORAGE_BOOT_POLL_INTERVAL_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));

        remaining_ms = storage_deadline_remaining_ms(deadline_us);
        if (remaining_ms == 0U) {
            return ESP_ERR_TIMEOUT;
        }
        const uint32_t poll_timeout_ms = remaining_ms < D1L_STORAGE_BOOT_POLL_TIMEOUT_MS ?
            remaining_ms : D1L_STORAGE_BOOT_POLL_TIMEOUT_MS;
        esp_err_t poll_ret = d1l_storage_status_refresh(poll_timeout_ms);
        if (poll_ret != ESP_OK && poll_ret != ESP_ERR_TIMEOUT) {
            return poll_ret;
        }
        last_ret = poll_ret;
    }

    if (last_ret == ESP_ERR_TIMEOUT && strcmp(s_status.sd_state, "mount_pending") != 0) {
        return ESP_OK;
    }
    return last_ret;
}

static esp_err_t poll_mount_pending(void)
{
    const uint32_t attempt_budget_ms =
        D1L_STORAGE_BOOT_POLL_INTERVAL_MS + D1L_STORAGE_BOOT_POLL_TIMEOUT_MS;
    const int64_t deadline_us = esp_timer_get_time() +
        ((int64_t)D1L_STORAGE_BOOT_POLL_ATTEMPTS * attempt_budget_ms * 1000LL);
    return poll_mount_pending_until(deadline_us);
}

static bool remount_timeout_needs_bridge_reset(void)
{
    return strcmp(s_status.sd_state, "protocol_pending") == 0 ||
           strcmp(s_status.sd_state, "rp2040_unavailable") == 0 ||
           !s_status.rp2040_bridge_ready ||
           !s_status.rp2040_sd_protocol_supported;
}

static bool request_bridge_reset_remount_recovery(void)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    const bool queued = !s_force_nvs && !s_manager_reset_recovery_active;
    if (queued) {
        s_manager_reset_bridge_requested = true;
        s_manager_remount_requested = true;
    }
    portEXIT_CRITICAL(&s_manager_control_lock);
    if (!queued) {
        return false;
    }
    s_status.manager_backoff_ms = 0;
    set_manager_state(D1L_STORAGE_MANAGER_BRIDGE_WAIT);
    return true;
}

static esp_err_t reset_bridge_and_remount_blocking(int64_t deadline_us)
{
    if (!manager_control_begin_manual_reset()) {
        return ESP_ERR_INVALID_STATE;
    }
    set_manager_state(D1L_STORAGE_MANAGER_BRIDGE_WAIT);

    const uint32_t reset_duration_ms = D1L_STORAGE_MANAGER_RESET_HOLD_MS +
                                       D1L_STORAGE_MANAGER_RESET_SETTLE_MS;
    if (storage_deadline_remaining_ms(deadline_us) <= reset_duration_ms) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret =
        d1l_rp2040_bridge_reset(D1L_STORAGE_MANAGER_RESET_HOLD_MS,
                                D1L_STORAGE_MANAGER_RESET_SETTLE_MS);
    manager_control_satisfy_reset();
    d1l_storage_status_note_rp2040(ret);
    if (ret != ESP_OK) {
        return ret;
    }
    if (storage_deadline_remaining_ms(deadline_us) == 0U) {
        return ESP_ERR_TIMEOUT;
    }

    set_manager_state(D1L_STORAGE_MANAGER_PING);
    for (uint32_t attempt = 0; attempt < D1L_STORAGE_MANAGER_RECOVERY_PING_ATTEMPTS;
         ++attempt) {
        uint32_t remaining_ms = storage_deadline_remaining_ms(deadline_us);
        if (remaining_ms == 0U) {
            return ESP_ERR_TIMEOUT;
        }
        const uint32_t ping_timeout_ms =
            remaining_ms < D1L_STORAGE_RP2040_SD_BOOT_PROBE_TIMEOUT_MS ?
            remaining_ms : D1L_STORAGE_RP2040_SD_BOOT_PROBE_TIMEOUT_MS;
        d1l_rp2040_ping_t ping = {0};
        ret = d1l_rp2040_bridge_ping(&ping, ping_timeout_ms);
        if (ret == ESP_OK) {
            d1l_storage_status_note_valid_bridge_response();
            break;
        }
        d1l_storage_status_note_rp2040(ret);
        if (attempt + 1U < D1L_STORAGE_MANAGER_RECOVERY_PING_ATTEMPTS) {
            remaining_ms = storage_deadline_remaining_ms(deadline_us);
            if (remaining_ms == 0U) {
                return ESP_ERR_TIMEOUT;
            }
            const uint32_t delay_ms =
                remaining_ms < D1L_STORAGE_MANAGER_RECOVERY_PING_INTERVAL_MS ?
                remaining_ms : D1L_STORAGE_MANAGER_RECOVERY_PING_INTERVAL_MS;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_storage_status_note_rp2040(ESP_OK);

    set_manager_state(D1L_STORAGE_MANAGER_MOUNT);
    uint32_t remaining_ms = storage_deadline_remaining_ms(deadline_us);
    if (remaining_ms == 0U) {
        return ESP_ERR_TIMEOUT;
    }
    ret = storage_status_mount(remaining_ms, true);
    if (ret == ESP_OK || ret == ESP_ERR_TIMEOUT) {
        esp_err_t poll_ret = poll_mount_pending_until(deadline_us);
        if (poll_ret != ESP_OK && ret == ESP_OK) {
            ret = poll_ret;
        }
    }
    return ret;
}

static void classify_storage_manager_state(
    esp_err_t ret, bool retained_backend_paused_for_failure)
{
    const bool force_nvs = manager_force_nvs_enabled();
    s_status.force_nvs = force_nvs;
    if (force_nvs) {
        apply_force_nvs_status();
        s_status.manager_backoff_ms = 0;
        return;
    }

    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        set_manager_state(D1L_STORAGE_MANAGER_ERROR_BACKOFF);
        s_status.manager_backoff_ms = D1L_STORAGE_MANAGER_BACKOFF_MS;
        return;
    }

    /* Cached card fields are useful diagnostics but cannot prove a safe
     * retained-store handoff after a failed manager exchange. The local
     * pause receipt covers the first timeout even when the global stale
     * threshold has not yet been reached; do not mutate that truthful flag. */
    if ((retained_backend_paused_for_failure && ret == ESP_ERR_TIMEOUT) ||
        s_status.bridge_status_stale) {
        set_manager_state(D1L_STORAGE_MANAGER_STATUS);
        s_status.manager_backoff_ms = 0;
        return;
    }

    if (storage_sd_ready_for_files()) {
        set_manager_state(D1L_STORAGE_MANAGER_READY_SD);
        s_status.manager_backoff_ms = 0;
    } else if (!s_status.rp2040_bridge_ready || !s_status.rp2040_sd_protocol_supported) {
        set_manager_state(D1L_STORAGE_MANAGER_BRIDGE_WAIT);
        s_status.manager_backoff_ms = D1L_STORAGE_MANAGER_BACKOFF_MS;
    } else if (!s_status.sd_present) {
        set_manager_state(D1L_STORAGE_MANAGER_NO_CARD);
        s_status.manager_backoff_ms = 0;
    } else if (s_status.sd_needs_fat32 ||
               strcmp(s_status.sd_state, "not_fat32_or_unmountable") == 0) {
        set_manager_state(D1L_STORAGE_MANAGER_NEEDS_FAT32);
        s_status.manager_backoff_ms = 0;
    } else {
        set_manager_state(D1L_STORAGE_MANAGER_READY_VOLATILE);
        s_status.manager_backoff_ms = 0;
    }
}

static void storage_manager_run_once_owned(void)
{
    s_status.manager_running = true;
    s_status.manager_attempt++;
    bool force_nvs = false;
    bool manual_reset_requested = false;
    bool manual_remount_requested = false;
    manager_control_take(&force_nvs, &manual_reset_requested,
                         &manual_remount_requested);
    s_status.force_nvs = force_nvs;
    if (force_nvs) {
        apply_force_nvs_status();
        return;
    }

    bool auto_reset_performed = false;
    bool ping_already_valid = false;
    if (!manual_reset_requested && initial_bridge_reset_is_pending()) {
        set_manager_state(D1L_STORAGE_MANAGER_PING);
        d1l_rp2040_ping_t final_ping = {0};
        const esp_err_t final_ping_ret = d1l_rp2040_bridge_ping(
            &final_ping, D1L_STORAGE_RP2040_SD_BOOT_PROBE_TIMEOUT_MS);
        if (final_ping_ret == ESP_OK) {
            d1l_storage_status_note_valid_bridge_response();
            ping_already_valid = true;
        }
    }
    if (manager_force_nvs_enabled()) {
        apply_force_nvs_status();
        return;
    }
    if (!ping_already_valid && !manual_reset_requested &&
        claim_initial_bridge_reset()) {
        set_manager_state(D1L_STORAGE_MANAGER_BRIDGE_WAIT);
        esp_err_t reset_ret =
            d1l_rp2040_bridge_reset(D1L_STORAGE_MANAGER_RESET_HOLD_MS,
                                    D1L_STORAGE_MANAGER_RESET_SETTLE_MS);
        d1l_storage_status_note_rp2040(reset_ret);
        if (reset_ret != ESP_OK) {
            classify_storage_manager_state(reset_ret, false);
            return;
        }
        auto_reset_performed = true;
        manager_control_satisfy_reset();
    }

    if (manual_reset_requested) {
        if (manager_force_nvs_enabled()) {
            apply_force_nvs_status();
            return;
        }
        if (!manager_control_begin_manual_reset()) {
            if (manager_force_nvs_enabled()) {
                apply_force_nvs_status();
            }
            return;
        }
        set_manager_state(D1L_STORAGE_MANAGER_BRIDGE_WAIT);
        esp_err_t reset_ret =
            d1l_rp2040_bridge_reset(D1L_STORAGE_MANAGER_RESET_HOLD_MS,
                                    D1L_STORAGE_MANAGER_RESET_SETTLE_MS);
        d1l_storage_status_note_rp2040(reset_ret);
        if (reset_ret != ESP_OK) {
            classify_storage_manager_state(reset_ret, false);
            return;
        }
        manager_control_satisfy_reset();
        ping_already_valid = false;
    }

    if (manager_force_nvs_enabled()) {
        apply_force_nvs_status();
        return;
    }
    esp_err_t ret = ESP_OK;
    if (!ping_already_valid) {
        set_manager_state(D1L_STORAGE_MANAGER_PING);
        d1l_rp2040_ping_t ping = {0};
        ret = d1l_rp2040_bridge_ping(
            &ping, D1L_STORAGE_RP2040_SD_BOOT_PROBE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            if (initial_ping_timeout_requires_reset(ret)) {
                d1l_storage_status_note_rp2040(ret);
                set_manager_state(D1L_STORAGE_MANAGER_BRIDGE_WAIT);
                s_status.manager_backoff_ms = 0;
                return;
            }
            note_rp2040_exchange_failure(ret, false);
            pause_retained_sd_backend_for_manager_failure();
            classify_storage_manager_state(ret, true);
            return;
        }
        d1l_storage_status_note_valid_bridge_response();
    }

    if (manager_force_nvs_enabled()) {
        apply_force_nvs_status();
        return;
    }

    set_manager_state(D1L_STORAGE_MANAGER_STATUS);
    ret = d1l_storage_status_refresh(D1L_STORAGE_RP2040_SD_BOOT_PROBE_TIMEOUT_MS);

    if (manager_force_nvs_enabled()) {
        apply_force_nvs_status();
        return;
    }

    const bool remount_requested = manual_remount_requested ||
                                   manual_reset_requested ||
                                   auto_reset_performed;
    if ((ret == ESP_OK || ret == ESP_ERR_TIMEOUT) &&
        (remount_requested ||
         strcmp(s_status.sd_state, "mount_required") == 0 ||
         strcmp(s_status.sd_state, "mount_pending") == 0)) {
        set_manager_state(D1L_STORAGE_MANAGER_MOUNT);
        ret = d1l_storage_status_mount(remount_requested ?
                                       D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS :
                                       D1L_STORAGE_RP2040_SD_BOOT_PROBE_TIMEOUT_MS);
        /* Routine manager work is asynchronous. A mount-pending reply is
         * observed again on the next bounded pass instead of holding bridge
         * quiescence for the full 30-second boot polling budget. Explicit
         * serial remount acceptance retains its blocking deadline path. */
        if (ret == ESP_ERR_TIMEOUT && manual_remount_requested &&
            !manual_reset_requested && !auto_reset_performed &&
            remount_timeout_needs_bridge_reset()) {
            request_bridge_reset_remount_recovery();
        }
    }

    const bool retained_backend_paused_for_failure = ret != ESP_OK;
    if (retained_backend_paused_for_failure) {
        pause_retained_sd_backend_for_manager_failure();
    }
    classify_storage_manager_state(ret, retained_backend_paused_for_failure);
}

static void storage_manager_run_once(void)
{
    if (!manager_sequence_try_take()) {
        return;
    }

    /* Match the serial remount lock order. No background status/mount/reset
     * exchange may race Public/DM/packet/route reconciliation on the same
     * RP2040 bridge. Routine polling waits for the current worker pass instead
     * of cancelling it: packet-primary reads can exceed the two-second manager
     * cadence, so preempting every pass would restart reconciliation forever
     * and prevent the route store later in the pass from running. Urgent serial
     * remount/reboot paths retain the preemptive quiesce API. */
    const esp_err_t retained_quiesce_ret =
        d1l_route_store_worker_quiesce_wait_begin(
            D1L_STORAGE_MANAGER_PASSIVE_QUIESCE_TIMEOUT_MS);
    if (retained_quiesce_ret != ESP_OK) {
        s_status.last_error = retained_quiesce_ret;
        s_status.manager_backoff_ms = 0;
        s_status.setup_action = "wait_for_retained_worker";
        s_status.note =
            "Storage manager is waiting for retained persistence to quiesce before bridge access";
        set_manager_state(D1L_STORAGE_MANAGER_STATUS);
        manager_sequence_give();
        return;
    }
    storage_manager_run_once_owned();
    manager_control_finish_reset_recovery();
    d1l_route_store_worker_quiesce_end();
    manager_sequence_give();
}

static uint32_t storage_manager_pause_delay_ms(void)
{
    portENTER_CRITICAL(&s_manager_pause_lock);
    const int64_t pause_until_us = s_manager_pause_until_us;
    portEXIT_CRITICAL(&s_manager_pause_lock);

    if (pause_until_us <= 0) {
        return 0;
    }

    const int64_t now_us = esp_timer_get_time();
    if (pause_until_us <= now_us) {
        portENTER_CRITICAL(&s_manager_pause_lock);
        if (s_manager_pause_until_us <= now_us) {
            s_manager_pause_until_us = 0;
        }
        portEXIT_CRITICAL(&s_manager_pause_lock);
        return 0;
    }

    int64_t remaining_us = pause_until_us - now_us;
    const int64_t max_sleep_us = 1000000LL;
    if (remaining_us > max_sleep_us) {
        remaining_us = max_sleep_us;
    }
    return (uint32_t)((remaining_us + 999LL) / 1000LL);
}

static void storage_manager_task(void *arg)
{
    (void)arg;
    for (;;) {
        const uint32_t pause_delay_ms = storage_manager_pause_delay_ms();
        if (pause_delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(pause_delay_ms));
            continue;
        }
        storage_manager_run_once();
        const uint32_t delay_ms = s_status.manager_backoff_ms > 0 ?
            s_status.manager_backoff_ms : D1L_STORAGE_MANAGER_IDLE_INTERVAL_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t d1l_storage_boot_prepare(uint32_t timeout_ms)
{
    esp_err_t ret = d1l_storage_status_refresh(timeout_ms);
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        return ret;
    }

    const bool refresh_timed_out = ret == ESP_ERR_TIMEOUT;
    if (!refresh_timed_out &&
        strcmp(s_status.sd_state, "mount_required") != 0 &&
        strcmp(s_status.sd_state, "no_card") != 0) {
        /* A cached false no-card state must not suppress one safe mount try. */
        return ret;
    }

    /* The bridge may accept MOUNT even when its first STATUS reply was lost. */
    ret = d1l_storage_status_mount(timeout_ms);
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        return ret;
    }

    return poll_mount_pending();
}

esp_err_t d1l_storage_manager_start(void)
{
    if (!s_status.initialized) {
        (void)d1l_storage_status_init();
    }
    s_status.manager_running = s_storage_manager_task != NULL;
    if (s_storage_manager_task) {
        return ESP_OK;
    }
    BaseType_t created = xTaskCreate(storage_manager_task,
                                     "storage_manager",
                                     D1L_STORAGE_MANAGER_STACK_BYTES,
                                     NULL,
                                     3,
                                     &s_storage_manager_task);
    s_status.manager_running = created == pdPASS;
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t d1l_storage_manager_request_remount(void)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    s_force_nvs = false;
    if (!s_manager_reset_recovery_active) {
        s_manager_remount_requested = true;
    }
    portEXIT_CRITICAL(&s_manager_control_lock);
    s_status.force_nvs = false;
    return d1l_storage_manager_start();
}

esp_err_t d1l_storage_manager_reset_bridge(void)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    s_force_nvs = false;
    if (!s_manager_reset_recovery_active) {
        s_manager_reset_bridge_requested = true;
        s_manager_remount_requested = true;
    }
    portEXIT_CRITICAL(&s_manager_control_lock);
    s_status.force_nvs = false;
    return d1l_storage_manager_start();
}

void d1l_storage_manager_pause(uint32_t pause_ms)
{
    if (pause_ms == 0) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    const int64_t pause_until_us = now_us + ((int64_t)pause_ms * 1000LL);
    portENTER_CRITICAL(&s_manager_pause_lock);
    if (pause_until_us > s_manager_pause_until_us) {
        s_manager_pause_until_us = pause_until_us;
    }
    portEXIT_CRITICAL(&s_manager_pause_lock);
}

void d1l_storage_manager_resume(void)
{
    portENTER_CRITICAL(&s_manager_pause_lock);
    s_manager_pause_until_us = 0;
    portEXIT_CRITICAL(&s_manager_pause_lock);
}

esp_err_t d1l_storage_manager_quiesce_begin(uint32_t timeout_ms)
{
    if (timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_manager_quiesce_owner_lock);
    const bool already_quiesced = s_manager_quiesce_owner != NULL;
    portEXIT_CRITICAL(&s_manager_quiesce_owner_lock);
    if (already_quiesced) {
        return ESP_ERR_INVALID_STATE;
    }
    ensure_manager_sequence_mutex();
    if (!s_manager_sequence_mutex) {
        return ESP_ERR_NO_MEM;
    }

    const uint32_t pause_ms = timeout_ms > UINT32_MAX - 1000U ?
                                  UINT32_MAX : timeout_ms + 1000U;
    d1l_storage_manager_pause(pause_ms);
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (ticks == 0) {
        ticks = 1;
    }
    if (xSemaphoreTake(s_manager_sequence_mutex, ticks) != pdTRUE) {
        d1l_storage_manager_resume();
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_manager_quiesce_owner_lock);
    s_manager_quiesce_owner = xTaskGetCurrentTaskHandle();
    portEXIT_CRITICAL(&s_manager_quiesce_owner_lock);
    return ESP_OK;
}

void d1l_storage_manager_quiesce_end(void)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_manager_quiesce_owner_lock);
    if (s_manager_quiesce_owner != current) {
        portEXIT_CRITICAL(&s_manager_quiesce_owner_lock);
        return;
    }
    s_manager_quiesce_owner = NULL;
    portEXIT_CRITICAL(&s_manager_quiesce_owner_lock);
    manager_sequence_give();
    d1l_storage_manager_resume();
}

void d1l_storage_manager_force_nvs(bool force_nvs)
{
    portENTER_CRITICAL(&s_manager_control_lock);
    s_force_nvs = force_nvs;
    if (force_nvs) {
        s_manager_reset_bridge_requested = false;
        s_manager_remount_requested = false;
        s_manager_auto_reset_pending = false;
        sync_initial_recovery_status_locked();
    } else if (!s_manager_reset_recovery_active) {
        s_manager_remount_requested = true;
    }
    portEXIT_CRITICAL(&s_manager_control_lock);
    s_status.force_nvs = force_nvs;
    if (force_nvs) {
        apply_force_nvs_status();
    }
}

static esp_err_t storage_status_mount(uint32_t timeout_ms, bool force_bridge_mount)
{
    if (!s_status.initialized) {
        (void)d1l_storage_status_init();
    }

    if (!s_status.rp2040_bridge_required) {
        s_status.last_error = ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (manager_force_nvs_enabled()) {
        apply_force_nvs_status();
        return ESP_ERR_INVALID_STATE;
    }

    if (!force_bridge_mount && storage_sd_ready_for_files()) {
        s_status.last_error = ESP_OK;
        return ESP_OK;
    }

    d1l_rp2040_sd_status_t sd = {0};
    esp_err_t ret = d1l_rp2040_bridge_mount_sd(&sd, timeout_ms);
    apply_rp2040_sd_exchange_result(&sd, ret);
    if (manager_force_nvs_enabled()) {
        apply_force_nvs_status();
    }
    return ret;
}

esp_err_t d1l_storage_status_mount(uint32_t timeout_ms)
{
    return storage_status_mount(timeout_ms, false);
}

esp_err_t d1l_storage_status_remount_blocking(
    uint32_t timeout_ms, bool *out_retained_worker_quiesce_acquired)
{
    if (out_retained_worker_quiesce_acquired) {
        *out_retained_worker_quiesce_acquired = false;
    }
    if (!s_status.initialized) {
        (void)d1l_storage_status_init();
    }
    if (timeout_ms == 0U) {
        s_status.last_error = ESP_ERR_INVALID_ARG;
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t deadline_us = esp_timer_get_time() +
                                ((int64_t)timeout_ms * 1000LL);

    if (!s_status.rp2040_bridge_required) {
        s_status.last_error = ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* The serial blocking path and background manager share one high-level
     * owner. Wait within the command's existing absolute deadline so a
     * periodic manager cycle cannot make an otherwise healthy remount fail
     * merely because it owns the sequence at that instant. */
    uint32_t remaining_ms = storage_deadline_remaining_ms(deadline_us);
    const esp_err_t manager_sequence_ret = remaining_ms > 0U ?
        manager_sequence_take(remaining_ms) : ESP_ERR_TIMEOUT;
    if (manager_sequence_ret != ESP_OK) {
        s_status.last_error = manager_sequence_ret;
        return manager_sequence_ret;
    }

    remaining_ms = storage_deadline_remaining_ms(deadline_us);
    const esp_err_t retained_quiesce_ret = remaining_ms > 0U ?
        d1l_route_store_worker_quiesce_begin(remaining_ms) : ESP_ERR_TIMEOUT;
    if (retained_quiesce_ret != ESP_OK) {
        s_status.last_error = retained_quiesce_ret;
        manager_sequence_give();
        return retained_quiesce_ret;
    }
    if (out_retained_worker_quiesce_acquired) {
        *out_retained_worker_quiesce_acquired = true;
    }

    portENTER_CRITICAL(&s_manager_control_lock);
    s_force_nvs = false;
    s_manager_reset_bridge_requested = false;
    s_manager_remount_requested = false;
    portEXIT_CRITICAL(&s_manager_control_lock);
    s_status.force_nvs = false;

    d1l_storage_status_note_rp2040(ESP_OK);

    set_manager_state(D1L_STORAGE_MANAGER_MOUNT);
    remaining_ms = storage_deadline_remaining_ms(deadline_us);
    esp_err_t ret = remaining_ms > 0U ?
        storage_status_mount(remaining_ms, true) : ESP_ERR_TIMEOUT;
    if (ret == ESP_OK || ret == ESP_ERR_TIMEOUT) {
        esp_err_t poll_ret = poll_mount_pending_until(deadline_us);
        if (poll_ret != ESP_OK && ret == ESP_OK) {
            ret = poll_ret;
        }
    }
    if (ret == ESP_ERR_TIMEOUT && remount_timeout_needs_bridge_reset()) {
        ret = reset_bridge_and_remount_blocking(deadline_us);
    }

    classify_storage_manager_state(ret, false);
    manager_control_finish_reset_recovery();
    d1l_route_store_worker_quiesce_end();
    manager_sequence_give();
    return ret;
}

void d1l_storage_status(d1l_storage_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    if (!s_status.initialized) {
        (void)d1l_storage_status_init();
    }
    refresh_retained_sd_health(&s_status);
    portENTER_CRITICAL(&s_manager_control_lock);
    s_status.force_nvs = s_force_nvs;
    sync_initial_recovery_status_locked();
    *out_status = s_status;
    portEXIT_CRITICAL(&s_manager_control_lock);
}
