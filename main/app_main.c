#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "d1l_config.h"
#include "app/release_profile.h"
#include "app/settings_model.h"
#include "diagnostics/crash_log.h"
#include "diagnostics/event_log.h"
#include "diagnostics/health_monitor.h"
#include "hal/indicator_board.h"
#include "hal/backlight.h"
#include "hal/display_preferences.h"
#include "hal/rp2040_bridge.h"
#include "mesh/channel_message_coordinator.h"
#include "mesh/channel_store.h"
#include "mesh/contact_store.h"
#include "mesh/dm_store.h"
#include "mesh/message_store.h"
#include "mesh/node_store.h"
#include "mesh/packet_log.h"
#include "mesh/read_state.h"
#include "mesh/route_store.h"
#include "mesh/route_store_worker.h"
#include "mesh/meshcore_service.h"
#include "map/map_prefetch_service.h"
#include "platform/time_service.h"
#include "platform/secure_random.h"
#include "storage/retained_blob_store.h"
#include "storage/factory_reset.h"
#include "storage/storage_status.h"
#include "update/update_manager.h"
#include "ui/ui_phase1.h"
#include "comms/connectivity_manager.h"
#include "comms/observer_manager.h"
#include "comms/usb_console.h"

static const char *TAG = "d1l_main";

void app_main(void)
{
    /* This must remain the first application-owned initialization: the secure
     * random service temporarily owns the boot entropy source before any ADC,
     * radio, Wi-Fi, Bluetooth, board, storage, or UI subsystem can start. */
    esp_err_t secure_random_ret = d1l_secure_random_init();
    if (secure_random_ret != ESP_OK) {
        ESP_LOGE(TAG, "secure random unavailable; identity/channel creation disabled: %s",
                 esp_err_to_name(secure_random_ret));
    }
    esp_err_t nvs_ret = nvs_flash_init();
    d1l_health_monitor_init(nvs_ret);
    d1l_event_log_init();
    d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "system", "boot",
                         "application startup");
    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable; preserving persisted data: %s",
                 esp_err_to_name(nvs_ret));
    }
    /* Inspect the default-NVS reset journal before initializing the retained
     * store layer. A corrupt/future/error journal gets a producer-silent USB
     * recovery console; no RF, connectivity, SD bridge, or message store is
     * allowed to start in that boot lifetime. */
    d1l_factory_reset_status_t factory_reset_status = {0};
    esp_err_t factory_reset_ret =
        d1l_factory_reset_inspect(&factory_reset_status);
    if (factory_reset_ret != ESP_OK) {
        ESP_LOGE(TAG, "factory reset journal quarantined before stores: %s",
                 esp_err_to_name(factory_reset_ret));
        printf("{\"schema\":%d,\"event\":\"factory_reset_recovery\","
               "\"ok\":false,\"phase\":\"%s\",\"error\":\"%s\","
               "\"recovery_console_started\":true,"
               "\"producers_started\":false,\"rf_started\":false,"
               "\"connectivity_started\":false,\"stores_started\":false,"
               "\"sd_touched\":false}\n",
               D1L_CONSOLE_SCHEMA,
               d1l_factory_reset_phase_name(factory_reset_status.phase),
               esp_err_to_name(factory_reset_ret));
        d1l_usb_console_run_factory_reset_recovery(&factory_reset_status);
        return;
    }
    esp_err_t retained_nvs_ret = d1l_retained_blob_store_init();
    if (retained_nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "retained NVS unavailable; preserving legacy mirrors: %s",
                 esp_err_to_name(retained_nvs_ret));
    }
    /* Resume a confirmed reset before time/settings/store producers can
     * recreate a key cleared by an earlier interrupted pass. The coordinator
     * starts every retry from domain zero and never touches removable SD. */
    factory_reset_ret = d1l_factory_reset_resume(&factory_reset_status);
    if (factory_reset_ret != ESP_OK) {
        ESP_LOGE(TAG, "factory reset recovery stopped before producers: %s",
                 esp_err_to_name(factory_reset_ret));
        printf("{\"schema\":%d,\"event\":\"factory_reset_recovery\","
               "\"ok\":false,\"phase\":\"%s\",\"attempt\":%lu,"
               "\"domains_completed\":%lu,\"domains_total\":%lu,"
               "\"failed_domain\":\"%s\",\"error\":\"%s\","
               "\"retry_on_next_boot\":true,\"global_atomic\":false,"
               "\"physical_flash_scrubbed\":false,\"sd_touched\":false,"
               "\"producers_started\":false}\n",
               D1L_CONSOLE_SCHEMA,
               d1l_factory_reset_phase_name(factory_reset_status.phase),
               (unsigned long)factory_reset_status.attempt_count,
               (unsigned long)factory_reset_status.domains_completed,
               (unsigned long)factory_reset_status.domains_total,
               factory_reset_status.last_failed_domain,
               esp_err_to_name(factory_reset_ret));
        d1l_usb_console_run_factory_reset_recovery(&factory_reset_status);
        return;
    }
    esp_err_t time_ret = d1l_time_service_init();
    if (time_ret != ESP_OK) {
        ESP_LOGE(TAG, "truthful time protocol guard unavailable: %s",
                 esp_err_to_name(time_ret));
    }

    printf("{\"schema\":%d,\"event\":\"boot\",\"firmware\":\"%s\",\"version\":\"%s\",\"target\":\"seeed_indicator_d1l\",\"release_profile\":\"%s\",\"sd_history_mode\":\"%s\",\"boot_nonce\":%lu,\"secure_random_ready\":%s,\"secure_random_error\":\"%s\",\"nvs_ready\":%s,\"nvs_error\":\"%s\",\"retained_nvs_marker_ready\":%s,\"retained_nvs_markers_complete\":%s,\"retained_nvs_anchor_ready\":%s,\"retained_nvs_sentinel_ready\":%s,\"retained_nvs_external_init_required\":%s,\"retained_nvs_initialized_this_boot\":%s,\"retained_nvs_ready\":%s,\"retained_nvs_init_error\":\"%s\",\"retained_nvs_migration_error\":\"%s\"}\n",
           D1L_CONSOLE_SCHEMA, D1L_FIRMWARE_NAME, D1L_FIRMWARE_VERSION,
           d1l_release_profile_name(),
           d1l_release_sd_history_mode_name(),
           (unsigned long)d1l_health_monitor_boot_nonce(),
           d1l_secure_random_ready() ? "true" : "false",
           esp_err_to_name(d1l_secure_random_status()),
           nvs_ret == ESP_OK ? "true" : "false", esp_err_to_name(nvs_ret),
           d1l_retained_blob_store_nvs_marker_ready() ? "true" : "false",
           d1l_retained_blob_store_nvs_markers_complete() ? "true" : "false",
           d1l_retained_blob_store_nvs_anchor_ready() ? "true" : "false",
           d1l_retained_blob_store_nvs_sentinel_ready() ? "true" : "false",
           d1l_retained_blob_store_nvs_external_init_required() ?
               "true" : "false",
           d1l_retained_blob_store_nvs_initialized_this_boot() ? "true" : "false",
           d1l_retained_blob_store_nvs_ready() ? "true" : "false",
           esp_err_to_name(d1l_retained_blob_store_nvs_error()),
           esp_err_to_name(d1l_retained_blob_store_nvs_migration_error()));

    /*
     * Bring up the LCD before retained SD history is read over the 115200-baud
     * RP2040 bridge. A populated card can take tens of seconds to replay, and
     * leaving the panel at its power-on blue during that bounded work makes a
     * healthy boot indistinguishable from a hang.
     */
    esp_err_t board_ret = d1l_board_init();
    if (board_ret == ESP_OK) {
        const esp_err_t splash_ret = d1l_board_display_boot_splash();
        if (splash_ret != ESP_OK) {
            ESP_LOGW(TAG, "DeskOS boot splash failed: %s",
                     esp_err_to_name(splash_ret));
        }
        ESP_LOGI(TAG, "D1L board initialized; DeskOS boot splash visible");
    }

    esp_err_t storage_ret = d1l_storage_status_init();
    if (storage_ret != ESP_OK) {
        ESP_LOGW(TAG, "storage status init failed: %s", esp_err_to_name(storage_ret));
    }
    const bool sd_history_available = d1l_release_feature_available(
        D1L_RELEASE_FEATURE_SD_HISTORY);
    if (sd_history_available) {
        esp_err_t rp2040_ret = d1l_rp2040_bridge_init();
        d1l_storage_status_note_rp2040(rp2040_ret);
        if (rp2040_ret == ESP_OK) {
            esp_err_t sd_probe_ret = d1l_storage_status_refresh(
                D1L_STORAGE_RP2040_SD_BOOT_PROBE_TIMEOUT_MS);
            if (sd_probe_ret != ESP_OK && sd_probe_ret != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "boot SD status refresh failed: %s",
                         esp_err_to_name(sd_probe_ret));
            }
        } else {
            ESP_LOGW(TAG, "RP2040 bridge UART init failed: %s",
                     esp_err_to_name(rp2040_ret));
        }
    } else {
        d1l_storage_manager_force_nvs(true);
        ESP_LOGI(TAG, "SD history unavailable in release profile; using NVS");
    }
    esp_err_t crash_log_ret = d1l_crash_log_init();
    if (crash_log_ret != ESP_OK) {
        ESP_LOGW(TAG, "crash/reset log init failed: %s", esp_err_to_name(crash_log_ret));
    }

    esp_err_t settings_ret = d1l_settings_load();
    if (settings_ret != ESP_OK) {
        ESP_LOGW(TAG, "settings load failed: %s", esp_err_to_name(settings_ret));
    }
    esp_err_t display_preferences_ret = d1l_display_preferences_init();
    if (display_preferences_ret != ESP_OK) {
        ESP_LOGW(TAG, "display preferences load failed: %s",
                 esp_err_to_name(display_preferences_ret));
    }
    esp_err_t channel_store_ret = d1l_channel_store_init();
    if (channel_store_ret != ESP_OK) {
        ESP_LOGW(TAG, "channel store load failed: %s", esp_err_to_name(channel_store_ret));
    }
    esp_err_t message_store_ret = d1l_message_store_init();
    if (message_store_ret != ESP_OK) {
        ESP_LOGW(TAG, "message store load failed: %s", esp_err_to_name(message_store_ret));
    }
    if (channel_store_ret == ESP_OK && message_store_ret == ESP_OK) {
        esp_err_t channel_message_ret = d1l_channel_message_reconcile();
        if (channel_message_ret != ESP_OK) {
            ESP_LOGW(TAG, "channel message reconciliation failed: %s",
                     esp_err_to_name(channel_message_ret));
        }
    }
    esp_err_t dm_store_ret = d1l_dm_store_init();
    if (dm_store_ret != ESP_OK) {
        ESP_LOGW(TAG, "DM store load failed: %s", esp_err_to_name(dm_store_ret));
    }
    esp_err_t node_store_ret = d1l_node_store_init();
    if (node_store_ret != ESP_OK) {
        ESP_LOGW(TAG, "node store load failed: %s", esp_err_to_name(node_store_ret));
    }
    esp_err_t contact_store_ret = d1l_contact_store_init();
    if (contact_store_ret != ESP_OK) {
        ESP_LOGW(TAG, "contact store load failed: %s", esp_err_to_name(contact_store_ret));
    }
    esp_err_t read_state_ret = d1l_read_state_init();
    if (read_state_ret != ESP_OK) {
        ESP_LOGW(TAG, "read state load failed: %s", esp_err_to_name(read_state_ret));
    }
    esp_err_t route_store_ret = d1l_route_store_init();
    if (route_store_ret != ESP_OK) {
        ESP_LOGW(TAG, "route store load failed: %s", esp_err_to_name(route_store_ret));
    }
    esp_err_t packet_log_ret = d1l_packet_log_init();
    if (packet_log_ret != ESP_OK) {
        ESP_LOGW(TAG, "packet log load failed: %s", esp_err_to_name(packet_log_ret));
    }
    esp_err_t route_worker_ret = d1l_route_store_worker_start();
    if (route_worker_ret != ESP_OK) {
        ESP_LOGW(TAG, "retained persistence worker start failed: %s",
                 esp_err_to_name(route_worker_ret));
    }
    if (sd_history_available) {
        esp_err_t storage_manager_ret = d1l_storage_manager_start();
        if (storage_manager_ret != ESP_OK) {
            ESP_LOGW(TAG, "storage manager start failed: %s",
                     esp_err_to_name(storage_manager_ret));
        }
    }
    d1l_meshcore_service_init();

    if (board_ret == ESP_OK) {
        d1l_display_preferences_t display_preferences = {0};
        d1l_display_preferences_get(&display_preferences);
        d1l_settings_t public_settings = {0};
        (void)d1l_settings_public_snapshot(&public_settings);
        const uint8_t startup_brightness = public_settings.night_mode ?
            25U : display_preferences.brightness_percent;
        const esp_err_t backlight_ret =
            d1l_backlight_set_percent(startup_brightness);
        if (backlight_ret != ESP_OK) {
            ESP_LOGW(TAG, "saved backlight apply failed: %s",
                     esp_err_to_name(backlight_ret));
        }
    }
    if (board_ret == ESP_OK) {
        esp_err_t mesh_rx_ret = d1l_meshcore_service_start_rx_async();
        if (mesh_rx_ret != ESP_OK) {
            ESP_LOGW(TAG, "MeshCore RX start queue failed: %s", esp_err_to_name(mesh_rx_ret));
        } else {
            /*
             * Public channel packets do not carry a sender name. Announce the
             * retained signed identity once after RX starts so nearby clients
             * can attribute subsequent messages to this D1L.
             */
            esp_err_t boot_advert_ret =
                d1l_meshcore_service_request_boot_advert(true);
            if (boot_advert_ret != ESP_OK) {
                ESP_LOGW(TAG, "MeshCore boot advert failed: %s",
                         esp_err_to_name(boot_advert_ret));
            }
        }
    }
    esp_err_t connectivity_ret = d1l_connectivity_init();
    if (connectivity_ret != ESP_OK) {
        ESP_LOGW(TAG, "connectivity policy init failed: %s", esp_err_to_name(connectivity_ret));
    }
    esp_err_t map_prefetch_ret = d1l_map_prefetch_service_init();
    if (map_prefetch_ret != ESP_OK) {
        ESP_LOGW(TAG, "background map service init failed: %s",
                 esp_err_to_name(map_prefetch_ret));
    }
    esp_err_t observer_ret = d1l_observer_manager_init();
    if (observer_ret != ESP_OK && observer_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "observer manager init failed: %s",
                 esp_err_to_name(observer_ret));
    }
    esp_err_t update_ret = d1l_update_manager_init();
    if (update_ret != ESP_OK && update_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "signed update manager init failed: %s",
                 esp_err_to_name(update_ret));
    }

    esp_err_t ui_ret = ESP_ERR_INVALID_STATE;
    if (board_ret == ESP_OK) {
        ESP_LOGI(TAG, "D1L full UI starting");
        ui_ret = d1l_ui_phase1_start();
        if (ui_ret != ESP_OK) {
            ESP_LOGE(TAG, "D1L UI startup failed: %s",
                     esp_err_to_name(ui_ret));
            printf("{\"schema\":%d,\"event\":\"ui_init\",\"ok\":false,"
                   "\"code\":\"%s\",\"hint\":\"UI task allocation failed\"}\n",
                   D1L_CONSOLE_SCHEMA, esp_err_to_name(ui_ret));
        }
    } else {
        ESP_LOGE(TAG, "D1L board init failed: %s", esp_err_to_name(board_ret));
        printf("{\"schema\":%d,\"event\":\"board_init\",\"ok\":false,\"code\":\"%s\",\"hint\":\"verify D1L hardware, I2C expander, and display power\"}\n",
               D1L_CONSOLE_SCHEMA, esp_err_to_name(board_ret));
    }

    const esp_err_t update_boot_health =
        nvs_ret == ESP_OK && settings_ret == ESP_OK &&
                board_ret == ESP_OK && ui_ret == ESP_OK ?
            ESP_OK : ESP_FAIL;
    esp_err_t update_confirm_ret =
        d1l_update_boot_confirm(update_boot_health);
    if (update_confirm_ret != ESP_OK) {
        ESP_LOGW(TAG, "update boot confirmation failed: %s",
                 esp_err_to_name(update_confirm_ret));
    }

    d1l_usb_console_run();
}
