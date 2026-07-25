#include "update_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/release_profile.h"
#include "comms/connectivity_manager.h"
#include "diagnostics/event_log.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/rp2040_bridge.h"
#include "mbedtls/sha256.h"
#include "mesh/ed25519_canonical.h"
#include "mesh/route_store_worker.h"
#include "nvs.h"
#include "storage/storage_status.h"
#include "update_signing_key.h"

#include "ed_25519.h"

#ifndef D1L_PARTITION_TABLE_SHA256
#error "D1L_PARTITION_TABLE_SHA256 must bind updates to the built partition table"
#endif

#define D1L_UPDATE_NAMESPACE "d1l_update"
#define D1L_UPDATE_MANIFEST_MAX 768U
#define D1L_UPDATE_SIGNATURE_BYTES 64U
#define D1L_UPDATE_TASK_STACK_BYTES 8192U
#define D1L_UPDATE_FILE_TIMEOUT_MS 5000U
#define D1L_UPDATE_REBOOT_DRAIN_MS 300U
#define D1L_UPDATE_MIN_INTERNAL_HEAP_BYTES 32768U
#define D1L_UPDATE_PROJECT_NAME "meshcore_deskos_d1l"
#define D1L_UPDATE_MANIFEST_HEADER "D1L-UPDATE-MANIFEST-V1"
#define D1L_UPDATE_PRODUCT "MeshCore DeskOS D1L"
#define D1L_UPDATE_TARGET "seeed_indicator_d1l"

typedef struct {
    char product[32];
    char target[32];
    char version[D1L_UPDATE_VERSION_LEN];
    char source_sha[D1L_UPDATE_SOURCE_SHA_LEN];
    char partition_sha256[65];
    char image_sha256[65];
    uint32_t image_size;
    uint32_t security_sequence;
    char signer_key_id[32];
} d1l_update_manifest_t;

static const char *TAG = "d1l_update";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_update_status_t s_status = {
    .state = D1L_UPDATE_STATE_IDLE,
    .rollback_enabled = true,
};
static TaskHandle_t s_task;
static bool s_install_requested;
static bool s_cancel_requested;
static bool s_reboot_prepared;

static void secure_zero(void *value, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)value;
    while (bytes && size-- > 0U) {
        *bytes++ = 0U;
    }
}

static void set_state(d1l_update_state_t state, esp_err_t error,
                      uint8_t progress)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.last_error = error;
    s_status.progress_percent = progress;
    s_status.cancel_allowed =
        state == D1L_UPDATE_STATE_INSPECTING ||
        state == D1L_UPDATE_STATE_VERIFYING_SIGNATURE ||
        state == D1L_UPDATE_STATE_VERIFYING_IMAGE;
    s_status.reboot_required =
        state == D1L_UPDATE_STATE_REBOOT_REQUIRED;
    portEXIT_CRITICAL(&s_lock);
}

static bool cancel_requested(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool requested = s_cancel_requested;
    portEXIT_CRITICAL(&s_lock);
    return requested;
}

static bool exact_lower_hex(const char *text, size_t digits)
{
    if (!text || strlen(text) != digits) {
        return false;
    }
    for (size_t i = 0U; i < digits; ++i) {
        const char ch = text[i];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool parse_u32(const char *text, uint32_t *out)
{
    if (!text || !out || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (!end || *end != '\0' || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool take_line(char **cursor, const char **out_line)
{
    if (!cursor || !*cursor || !out_line || **cursor == '\0') {
        return false;
    }
    char *line = *cursor;
    char *newline = strchr(line, '\n');
    if (!newline) {
        return false;
    }
    *newline = '\0';
    *cursor = newline + 1U;
    *out_line = line;
    return true;
}

static bool copy_field(const char *line, const char *prefix,
                       char *out, size_t out_size)
{
    if (!line || !prefix || !out || out_size == 0U) {
        return false;
    }
    const size_t prefix_len = strlen(prefix);
    if (strncmp(line, prefix, prefix_len) != 0 ||
        line[prefix_len] == '\0' ||
        strlen(&line[prefix_len]) >= out_size) {
        return false;
    }
    snprintf(out, out_size, "%s", &line[prefix_len]);
    return true;
}

static bool parse_manifest(const uint8_t *bytes, size_t length,
                           d1l_update_manifest_t *out)
{
    if (!bytes || !out || length == 0U ||
        length > D1L_UPDATE_MANIFEST_MAX || bytes[length - 1U] != '\n') {
        return false;
    }
    char buffer[D1L_UPDATE_MANIFEST_MAX + 1U] = {0};
    for (size_t i = 0U; i < length; ++i) {
        if (bytes[i] == '\0' || bytes[i] == '\r' ||
            (bytes[i] < 0x20U && bytes[i] != '\n') ||
            bytes[i] > 0x7EU) {
            return false;
        }
        buffer[i] = (char)bytes[i];
    }
    memset(out, 0, sizeof(*out));
    char *cursor = buffer;
    const char *line = NULL;
    if (!take_line(&cursor, &line) ||
        strcmp(line, D1L_UPDATE_MANIFEST_HEADER) != 0 ||
        !take_line(&cursor, &line) ||
        !copy_field(line, "product=", out->product, sizeof(out->product)) ||
        !take_line(&cursor, &line) ||
        !copy_field(line, "target=", out->target, sizeof(out->target)) ||
        !take_line(&cursor, &line) ||
        !copy_field(line, "version=", out->version, sizeof(out->version)) ||
        !take_line(&cursor, &line) ||
        !copy_field(line, "source_sha=", out->source_sha,
                    sizeof(out->source_sha)) ||
        !take_line(&cursor, &line) ||
        !copy_field(line, "partition_table_sha256=",
                    out->partition_sha256,
                    sizeof(out->partition_sha256)) ||
        !take_line(&cursor, &line) ||
        !copy_field(line, "image_sha256=", out->image_sha256,
                    sizeof(out->image_sha256)) ||
        !take_line(&cursor, &line) ||
        strncmp(line, "image_size=", sizeof("image_size=") - 1U) != 0 ||
        !parse_u32(&line[sizeof("image_size=") - 1U],
                   &out->image_size) ||
        !take_line(&cursor, &line) ||
        strncmp(line, "security_sequence=",
                sizeof("security_sequence=") - 1U) != 0 ||
        !parse_u32(&line[sizeof("security_sequence=") - 1U],
                   &out->security_sequence) ||
        !take_line(&cursor, &line) ||
        !copy_field(line, "signer_key_id=", out->signer_key_id,
                    sizeof(out->signer_key_id)) ||
        *cursor != '\0') {
        secure_zero(out, sizeof(*out));
        return false;
    }
    const bool valid =
        strcmp(out->product, D1L_UPDATE_PRODUCT) == 0 &&
        strcmp(out->target, D1L_UPDATE_TARGET) == 0 &&
        strcmp(out->signer_key_id, D1L_UPDATE_SIGNER_KEY_ID) == 0 &&
        exact_lower_hex(out->source_sha, 40U) &&
        exact_lower_hex(out->partition_sha256, 64U) &&
        exact_lower_hex(out->image_sha256, 64U) &&
        out->image_size > 0U && out->security_sequence > 0U;
    if (!valid) {
        secure_zero(out, sizeof(*out));
    }
    return valid;
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_size)
{
    if (!hex || !out || strlen(hex) != out_size * 2U) {
        return false;
    }
    for (size_t i = 0U; i < out_size; ++i) {
        unsigned value = 0U;
        if (sscanf(&hex[i * 2U], "%2x", &value) != 1) {
            return false;
        }
        out[i] = (uint8_t)value;
    }
    return true;
}

static esp_err_t read_exact_file(const char *path, uint8_t *out,
                                 size_t length)
{
    if (!path || !out || length == 0U || length > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t offset = 0U;
    while (offset < length) {
        const size_t requested =
            length - offset < D1L_RP2040_FILE_CHUNK_MAX ?
                length - offset : D1L_RP2040_FILE_CHUNK_MAX;
        d1l_rp2040_file_result_t result = {0};
        const esp_err_t ret = d1l_rp2040_bridge_file_read(
            path, (uint32_t)offset, &out[offset], requested, &result,
            D1L_UPDATE_FILE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            return ret;
        }
        if (result.offset != offset || result.length == 0U ||
            result.length > requested || offset + result.length > length) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        offset += result.length;
        if (result.eof && offset != length) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

static esp_err_t inspect_file(const char *path, uint32_t *out_size)
{
    if (!path || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_rp2040_file_result_t result = {0};
    const esp_err_t ret = d1l_rp2040_bridge_file_stat(
        path, &result, D1L_UPDATE_FILE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!result.exists || result.is_directory || result.size == 0U) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_size = result.size;
    return ESP_OK;
}

static esp_err_t hash_image(const char *path, uint32_t image_size,
                            uint8_t digest[32])
{
    if (!path || image_size == 0U || !digest) {
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    if (mbedtls_sha256_starts(&context, 0) != 0) {
        mbedtls_sha256_free(&context);
        return ESP_FAIL;
    }
    uint8_t chunk[D1L_RP2040_FILE_CHUNK_MAX] = {0};
    uint32_t offset = 0U;
    esp_err_t ret = ESP_OK;
    while (offset < image_size) {
        if (cancel_requested()) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        const size_t requested =
            image_size - offset < sizeof(chunk) ?
                image_size - offset : sizeof(chunk);
        d1l_rp2040_file_result_t result = {0};
        ret = d1l_rp2040_bridge_file_read(
            path, offset, chunk, requested, &result,
            D1L_UPDATE_FILE_TIMEOUT_MS);
        if (ret != ESP_OK || result.offset != offset ||
            result.length == 0U || result.length > requested) {
            ret = ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : ret;
            break;
        }
        if (offset == 0U && chunk[0] != 0xE9U) {
            ret = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (mbedtls_sha256_update(&context, chunk, result.length) != 0) {
            ret = ESP_FAIL;
            break;
        }
        offset += result.length;
        portENTER_CRITICAL(&s_lock);
        s_status.bytes_verified = offset;
        s_status.progress_percent =
            (uint8_t)(10U + ((uint64_t)offset * 40U / image_size));
        portEXIT_CRITICAL(&s_lock);
        taskYIELD();
    }
    if (ret == ESP_OK &&
        mbedtls_sha256_finish(&context, digest) != 0) {
        ret = ESP_FAIL;
    }
    mbedtls_sha256_free(&context);
    secure_zero(chunk, sizeof(chunk));
    return ret;
}

static esp_err_t write_image(const char *path, uint32_t image_size,
                             const esp_partition_t *partition)
{
    if (!path || !partition || image_size == 0U ||
        image_size > partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_ota_handle_t ota_handle = 0U;
    esp_err_t ret = esp_ota_begin(
        partition, image_size, &ota_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t chunk[D1L_RP2040_FILE_CHUNK_MAX] = {0};
    uint32_t offset = 0U;
    while (offset < image_size) {
        const size_t requested =
            image_size - offset < sizeof(chunk) ?
                image_size - offset : sizeof(chunk);
        d1l_rp2040_file_result_t result = {0};
        ret = d1l_rp2040_bridge_file_read(
            path, offset, chunk, requested, &result,
            D1L_UPDATE_FILE_TIMEOUT_MS);
        if (ret != ESP_OK || result.offset != offset ||
            result.length == 0U || result.length > requested) {
            ret = ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : ret;
            break;
        }
        ret = esp_ota_write(ota_handle, chunk, result.length);
        if (ret != ESP_OK) {
            break;
        }
        offset += result.length;
        portENTER_CRITICAL(&s_lock);
        s_status.bytes_written = offset;
        s_status.progress_percent =
            (uint8_t)(50U + ((uint64_t)offset * 45U / image_size));
        portEXIT_CRITICAL(&s_lock);
        taskYIELD();
    }
    secure_zero(chunk, sizeof(chunk));
    if (ret != ESP_OK) {
        (void)esp_ota_abort(ota_handle);
        return ret;
    }
    return esp_ota_end(ota_handle);
}

static uint32_t load_highest_sequence(void)
{
    nvs_handle_t handle = 0U;
    uint32_t sequence = 0U;
    if (nvs_open(D1L_UPDATE_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u32(handle, "highest_seq", &sequence);
        nvs_close(handle);
    }
    return sequence;
}

static esp_err_t save_pending(const d1l_update_manifest_t *manifest)
{
    if (!manifest) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0U;
    esp_err_t ret =
        nvs_open(D1L_UPDATE_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = nvs_set_u32(handle, "pending_seq",
                          manifest->security_sequence);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "pending_sha", manifest->source_sha);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "pending_ver", manifest->version);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    return ret;
}

static void clear_pending(bool confirmed)
{
    nvs_handle_t handle = 0U;
    if (nvs_open(D1L_UPDATE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    uint32_t pending_sequence = 0U;
    (void)nvs_get_u32(handle, "pending_seq", &pending_sequence);
    if (confirmed && pending_sequence > 0U) {
        uint32_t highest = 0U;
        (void)nvs_get_u32(handle, "highest_seq", &highest);
        if (pending_sequence > highest) {
            (void)nvs_set_u32(handle, "highest_seq", pending_sequence);
        }
        (void)nvs_set_str(handle, "last_result", "confirmed");
    } else if (pending_sequence > 0U) {
        (void)nvs_set_str(handle, "last_result", "rolled_back");
    }
    (void)nvs_erase_key(handle, "pending_seq");
    (void)nvs_erase_key(handle, "pending_sha");
    (void)nvs_erase_key(handle, "pending_ver");
    (void)nvs_commit(handle);
    nvs_close(handle);
}

static esp_err_t run_install(void)
{
    set_state(D1L_UPDATE_STATE_INSPECTING, ESP_OK, 1U);
    uint32_t manifest_size = 0U;
    uint32_t signature_size = 0U;
    uint32_t image_size = 0U;
    esp_err_t ret = inspect_file(D1L_UPDATE_MANIFEST_PATH, &manifest_size);
    if (ret == ESP_OK) {
        ret = inspect_file(D1L_UPDATE_SIGNATURE_PATH, &signature_size);
    }
    if (ret == ESP_OK) {
        ret = inspect_file(D1L_UPDATE_IMAGE_PATH, &image_size);
    }
    if (ret != ESP_OK || manifest_size == 0U ||
        manifest_size > D1L_UPDATE_MANIFEST_MAX ||
        signature_size != D1L_UPDATE_SIGNATURE_BYTES) {
        return ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret;
    }
    if (cancel_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t manifest_bytes[D1L_UPDATE_MANIFEST_MAX] = {0};
    uint8_t signature[D1L_UPDATE_SIGNATURE_BYTES] = {0};
    d1l_update_manifest_t manifest = {0};
    ret = read_exact_file(
        D1L_UPDATE_MANIFEST_PATH, manifest_bytes, manifest_size);
    if (ret == ESP_OK) {
        ret = read_exact_file(
            D1L_UPDATE_SIGNATURE_PATH, signature, sizeof(signature));
    }
    if (ret != ESP_OK ||
        !parse_manifest(manifest_bytes, manifest_size, &manifest) ||
        image_size != manifest.image_size ||
        strcmp(manifest.partition_sha256,
               D1L_PARTITION_TABLE_SHA256) != 0) {
        ret = ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : ret;
        goto install_cleanup;
    }
    const uint32_t highest_sequence = load_highest_sequence();
    if (manifest.security_sequence <= highest_sequence &&
        highest_sequence != 0U) {
        ret = ESP_ERR_INVALID_VERSION;
        goto install_cleanup;
    }
    set_state(D1L_UPDATE_STATE_VERIFYING_SIGNATURE, ESP_OK, 5U);
    if (!d1l_ed25519_signature_s_is_canonical(signature) ||
        ed25519_verify(signature, manifest_bytes, manifest_size,
                       D1L_UPDATE_SIGNING_PUBLIC_KEY) != 1) {
        ret = ESP_ERR_INVALID_CRC;
        goto install_cleanup;
    }
    if (cancel_requested()) {
        ret = ESP_ERR_INVALID_STATE;
        goto install_cleanup;
    }

    portENTER_CRITICAL(&s_lock);
    s_status.image_size = manifest.image_size;
    s_status.security_sequence = manifest.security_sequence;
    s_status.highest_security_sequence = highest_sequence;
    snprintf(s_status.version, sizeof(s_status.version), "%s",
             manifest.version);
    snprintf(s_status.source_sha, sizeof(s_status.source_sha), "%s",
             manifest.source_sha);
    snprintf(s_status.signer_key_id, sizeof(s_status.signer_key_id), "%s",
             manifest.signer_key_id);
    portEXIT_CRITICAL(&s_lock);

    set_state(D1L_UPDATE_STATE_VERIFYING_IMAGE, ESP_OK, 10U);
    uint8_t expected_digest[32] = {0};
    uint8_t actual_digest[32] = {0};
    if (!hex_to_bytes(manifest.image_sha256, expected_digest,
                      sizeof(expected_digest))) {
        ret = ESP_ERR_INVALID_ARG;
        goto digest_cleanup;
    }
    ret = hash_image(
        D1L_UPDATE_IMAGE_PATH, manifest.image_size, actual_digest);
    if (ret == ESP_OK &&
        memcmp(expected_digest, actual_digest, sizeof(actual_digest)) != 0) {
        ret = ESP_ERR_INVALID_CRC;
    }
    if (ret != ESP_OK) {
        goto digest_cleanup;
    }

    const esp_partition_t *target =
        esp_ota_get_next_update_partition(NULL);
    if (!target || target->type != ESP_PARTITION_TYPE_APP ||
        (target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
        target->size < manifest.image_size) {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto digest_cleanup;
    }
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.target_partition, sizeof(s_status.target_partition),
             "%s", target->label);
    portEXIT_CRITICAL(&s_lock);

    set_state(D1L_UPDATE_STATE_WRITING, ESP_OK, 50U);
    ret = write_image(
        D1L_UPDATE_IMAGE_PATH, manifest.image_size, target);
    if (ret != ESP_OK) {
        goto digest_cleanup;
    }
    set_state(D1L_UPDATE_STATE_FINALIZING, ESP_OK, 96U);
    esp_app_desc_t descriptor = {0};
    if (esp_ota_get_partition_description(target, &descriptor) != ESP_OK ||
        strcmp(descriptor.project_name, D1L_UPDATE_PROJECT_NAME) != 0) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto digest_cleanup;
    }
    ret = save_pending(&manifest);
    if (ret == ESP_OK) {
        ret = esp_ota_set_boot_partition(target);
    }
    if (ret != ESP_OK) {
        clear_pending(false);
        goto digest_cleanup;
    }
    d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "update", "installed",
                         "signed image ready for controlled reboot");
    set_state(D1L_UPDATE_STATE_REBOOT_REQUIRED, ESP_OK, 100U);

digest_cleanup:
    secure_zero(expected_digest, sizeof(expected_digest));
    secure_zero(actual_digest, sizeof(actual_digest));
install_cleanup:
    secure_zero(signature, sizeof(signature));
    secure_zero(manifest_bytes, sizeof(manifest_bytes));
    secure_zero(&manifest, sizeof(manifest));
    return ret;
}

static void update_task(void *argument)
{
    (void)argument;
    for (;;) {
        bool requested = false;
        portENTER_CRITICAL(&s_lock);
        requested = s_install_requested;
        if (requested) {
            s_install_requested = false;
            s_status.install_requested = false;
            s_cancel_requested = false;
            s_status.bytes_verified = 0U;
            s_status.bytes_written = 0U;
        }
        portEXIT_CRITICAL(&s_lock);
        if (requested) {
            const esp_err_t ret = run_install();
            if (ret != ESP_OK) {
                if (ret == ESP_ERR_INVALID_STATE && cancel_requested()) {
                    set_state(D1L_UPDATE_STATE_CANCELLED, ESP_OK, 0U);
                    d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "update",
                                         "cancelled",
                                         "cancelled before flash write");
                } else {
                    set_state(D1L_UPDATE_STATE_ERROR, ret, 0U);
                    d1l_event_log_append(D1L_EVENT_LOG_LEVEL_ERROR, "update",
                                         "failed", esp_err_to_name(ret));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100U));
    }
}

esp_err_t d1l_update_boot_confirm(esp_err_t nvs_status)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t state_ret =
        esp_ota_get_state_partition(running, &image_state);
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.running_partition,
             sizeof(s_status.running_partition), "%s", running->label);
    portEXIT_CRITICAL(&s_lock);
    if (state_ret != ESP_OK ||
        image_state != ESP_OTA_IMG_PENDING_VERIFY) {
        clear_pending(false);
        return state_ret == ESP_ERR_NOT_SUPPORTED ||
                       state_ret == ESP_ERR_NOT_FOUND ?
                   ESP_OK : state_ret;
    }

    esp_app_desc_t descriptor = {0};
    const bool healthy =
        nvs_status == ESP_OK &&
        running->type == ESP_PARTITION_TYPE_APP &&
        (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
         running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) &&
        esp_ota_get_partition_description(running, &descriptor) == ESP_OK &&
        strcmp(descriptor.project_name, D1L_UPDATE_PROJECT_NAME) == 0 &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
            D1L_UPDATE_MIN_INTERNAL_HEAP_BYTES;
    if (!healthy) {
        d1l_event_log_append(D1L_EVENT_LOG_LEVEL_ERROR, "update",
                             "boot_rejected",
                             "new image failed boot diagnostics");
        (void)esp_ota_mark_app_invalid_rollback_and_reboot();
        esp_restart();
        return ESP_FAIL;
    }
    const esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ret == ESP_OK) {
        clear_pending(true);
        portENTER_CRITICAL(&s_lock);
        s_status.running_image_confirmed = true;
        portEXIT_CRITICAL(&s_lock);
        d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "update",
                             "boot_confirmed",
                             "new image accepted; rollback cancelled");
    }
    return ret;
}

esp_err_t d1l_update_manager_init(void)
{
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_SIGNED_UPDATE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_task) {
        return ESP_OK;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const uint32_t highest_sequence = load_highest_sequence();
    portENTER_CRITICAL(&s_lock);
    s_status.initialized = true;
    s_status.highest_security_sequence = highest_sequence;
    snprintf(s_status.signer_key_id, sizeof(s_status.signer_key_id), "%s",
             D1L_UPDATE_SIGNER_KEY_ID);
    if (running) {
        snprintf(s_status.running_partition,
                 sizeof(s_status.running_partition), "%s", running->label);
    }
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(update_task, "d1l_update",
                    D1L_UPDATE_TASK_STACK_BYTES, NULL, 3, &s_task) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "update", "ready",
                         "signed SD update service ready");
    return ESP_OK;
}

esp_err_t d1l_update_request_install(void)
{
    if (!s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_lock);
    const bool busy = s_install_requested ||
        (s_status.state >= D1L_UPDATE_STATE_INSPECTING &&
         s_status.state <= D1L_UPDATE_STATE_FINALIZING);
    if (!busy) {
        s_install_requested = true;
        s_status.install_requested = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return busy ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t d1l_update_cancel(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool allowed = s_status.cancel_allowed ||
        s_install_requested;
    if (allowed) {
        s_cancel_requested = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t d1l_update_prepare_reboot(void)
{
    d1l_update_status_t status = {0};
    d1l_update_status(&status);
    if (!status.reboot_required) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_lock);
    const bool already_prepared = s_reboot_prepared;
    portEXIT_CRITICAL(&s_lock);
    if (already_prepared) {
        return ESP_OK;
    }

    const esp_err_t storage_ret =
        d1l_storage_manager_quiesce_begin(5000U);
    if (storage_ret != ESP_OK) {
        return storage_ret;
    }
    const esp_err_t retained_ret =
        d1l_route_store_worker_quiesce_begin(5000U);
    if (retained_ret != ESP_OK) {
        d1l_storage_manager_quiesce_end();
        return retained_ret;
    }
    const esp_err_t bridge_ret =
        d1l_rp2040_bridge_quiesce_begin(5000U);
    if (bridge_ret != ESP_OK) {
        d1l_route_store_worker_quiesce_end();
        d1l_storage_manager_quiesce_end();
        return bridge_ret;
    }
    const esp_err_t connectivity_ret = d1l_connectivity_prepare_reboot();
    if (connectivity_ret != ESP_OK) {
        d1l_rp2040_bridge_quiesce_end();
        d1l_route_store_worker_quiesce_end();
        d1l_storage_manager_quiesce_end();
        return connectivity_ret;
    }
    portENTER_CRITICAL(&s_lock);
    s_reboot_prepared = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void d1l_update_execute_prepared_reboot(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool prepared = s_reboot_prepared;
    portEXIT_CRITICAL(&s_lock);
    if (!prepared) {
        esp_restart();
        for (;;) {
        }
    }
    d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "update", "reboot",
                         "booting verified update");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(D1L_UPDATE_REBOOT_DRAIN_MS));
    esp_rom_software_reset_system();
    for (;;) {
    }
}

esp_err_t d1l_update_reboot_to_installed(void)
{
    const esp_err_t ret = d1l_update_prepare_reboot();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_update_execute_prepared_reboot();
}

void d1l_update_status(d1l_update_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out_status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

const char *d1l_update_state_name(d1l_update_state_t state)
{
    switch (state) {
    case D1L_UPDATE_STATE_IDLE:
        return "idle";
    case D1L_UPDATE_STATE_INSPECTING:
        return "inspecting";
    case D1L_UPDATE_STATE_VERIFYING_SIGNATURE:
        return "verifying_signature";
    case D1L_UPDATE_STATE_VERIFYING_IMAGE:
        return "verifying_image";
    case D1L_UPDATE_STATE_WRITING:
        return "writing";
    case D1L_UPDATE_STATE_FINALIZING:
        return "finalizing";
    case D1L_UPDATE_STATE_REBOOT_REQUIRED:
        return "reboot_required";
    case D1L_UPDATE_STATE_CANCELLED:
        return "cancelled";
    case D1L_UPDATE_STATE_ROLLED_BACK:
        return "rolled_back";
    case D1L_UPDATE_STATE_ERROR:
        return "error";
    default:
        return "invalid";
    }
}
