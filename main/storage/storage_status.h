#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "storage/retained_blob_store.h"

#define D1L_STORAGE_SD_MOUNT_POINT "/sdcard"
#define D1L_STORAGE_SD_DATA_ROOT "/sdcard/deskos"
#define D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS 10000U
#define D1L_STORAGE_RP2040_SD_BOOT_PROBE_TIMEOUT_MS 1500U
/* Serial remount includes retained-worker quiesce, a mount/poll pass, and one
 * bounded RP2040 reset recovery under a single monotonic deadline. */
#define D1L_STORAGE_REMOUNT_TIMEOUT_MS 60000U

typedef struct {
    bool initialized;
    bool direct_supported;
    bool rp2040_bridge_required;
    bool rp2040_bridge_ready;
    bool rp2040_sd_protocol_supported;
    bool sd_present;
    bool sd_mounted;
    bool sd_data_root_ready;
    bool sd_needs_fat32;
    bool setup_required;
    bool setup_supported;
    bool data_enabled;
    bool file_ops_supported;
    bool atomic_rename_supported;
    bool response_truncated;
    uint32_t capacity_kb;
    uint32_t free_kb;
    uint32_t file_line_max;
    uint32_t file_chunk_max;
    uint32_t path_max;
    uint32_t sd_probe_error;
    uint32_t sd_probe_data;
    uint32_t sd_mount_error;
    uint32_t sd_mount_data;
    uint32_t manager_attempt;
    uint32_t manager_backoff_ms;
    uint32_t manager_initial_ping_timeouts;
    uint32_t bridge_status_refresh_failures;
    esp_err_t last_error;
    bool manager_running;
    bool force_nvs;
    bool manager_bridge_response_seen;
    bool manager_auto_reset_pending;
    bool manager_auto_reset_attempted;
    bool bridge_status_stale;
    bool sd_presence_stale;
    bool retained_sd_degraded;
    bool retained_backup_degraded;
    char sd_probe_power[8];
    char sd_probe_mode[16];
    const char *manager_state;
    const char *sd_state;
    const char *sd_interface;
    const char *sd_filesystem;
    const char *mount_point;
    const char *data_root;
    const char *data_backend;
    const char *message_store_backend;
    const char *dm_store_backend;
    const char *packet_log_backend;
    const char *route_store_backend;
    const char *node_store_backend;
    const char *map_tile_backend;
    const char *export_backend;
    const char *setup_action;
    const char *note;
    d1l_retained_blob_store_sd_stats_t
        retained_sd_stats[D1L_RETAINED_BLOB_STORE_COUNT];
} d1l_storage_status_t;

esp_err_t d1l_storage_status_init(void);
void d1l_storage_status_note_rp2040(esp_err_t rp2040_init_result);
/* Latches a parsed DeskOS bridge response and cancels any queued auto-reset. */
void d1l_storage_status_note_valid_bridge_response(void);
esp_err_t d1l_storage_boot_prepare(uint32_t timeout_ms);
esp_err_t d1l_storage_manager_start(void);
esp_err_t d1l_storage_manager_request_remount(void);
esp_err_t d1l_storage_manager_reset_bridge(void);
void d1l_storage_manager_pause(uint32_t pause_ms);
void d1l_storage_manager_resume(void);
/* Hold the manager sequence across a bounded shutdown/flush operation. */
esp_err_t d1l_storage_manager_quiesce_begin(uint32_t timeout_ms);
void d1l_storage_manager_quiesce_end(void);
void d1l_storage_manager_force_nvs(bool force_nvs);
esp_err_t d1l_storage_status_refresh(uint32_t timeout_ms);
esp_err_t d1l_storage_status_mount(uint32_t timeout_ms);
esp_err_t d1l_storage_status_remount_blocking(
    uint32_t timeout_ms, bool *out_retained_worker_quiesce_acquired);
void d1l_storage_status(d1l_storage_status_t *out_status);
