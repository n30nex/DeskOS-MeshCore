#include "export_store.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "storage/storage_status_policy.h"

#if D1L_ENABLE_QUALIFICATION_HOOKS
static bool result_path_set(d1l_export_canary_result_t *result,
                            const char *token)
{
    if (!result || !token || token[0] == '\0') {
        return false;
    }
    const int final_len = snprintf(result->path, sizeof(result->path),
                                   "exports/diagnostics/export-canary-%s.json",
                                   token);
    const int tmp_len = snprintf(result->tmp_path, sizeof(result->tmp_path),
                                 "exports/diagnostics/export-canary-%s.tmp",
                                 token);
    return final_len > 0 && tmp_len > 0 &&
           (size_t)final_len < sizeof(result->path) &&
           (size_t)tmp_len < sizeof(result->tmp_path);
}
#endif

static bool data_path_set(d1l_export_canary_result_t *result,
                          const char *token)
{
    if (!result || !token || token[0] == '\0') {
        return false;
    }
    const int final_len = snprintf(result->path, sizeof(result->path),
                                   "exports/data/data-export-%s.json",
                                   token);
    const int tmp_len = snprintf(result->tmp_path, sizeof(result->tmp_path),
                                 "exports/data/data-export-%s.tmp",
                                 token);
    return final_len > 0 && tmp_len > 0 &&
           (size_t)final_len < sizeof(result->path) &&
           (size_t)tmp_len < sizeof(result->tmp_path);
}

static bool diagnostics_path_set(d1l_export_canary_result_t *result,
                                 const char *token)
{
    if (!result || !token || token[0] == '\0') {
        return false;
    }
    const int final_len = snprintf(result->path, sizeof(result->path),
                                   "exports/diagnostics/diagnostic-export-%s.json",
                                   token);
    const int tmp_len = snprintf(result->tmp_path, sizeof(result->tmp_path),
                                 "exports/diagnostics/diagnostic-export-%s.tmp",
                                 token);
    return final_len > 0 && tmp_len > 0 &&
           (size_t)final_len < sizeof(result->path) &&
           (size_t)tmp_len < sizeof(result->tmp_path);
}

static void result_step(d1l_export_canary_result_t *result,
                        const char *step,
                        esp_err_t ret,
                        const d1l_rp2040_file_result_t *file)
{
    if (!result) {
        return;
    }
    snprintf(result->step, sizeof(result->step), "%s", step ? step : "unknown");
    result->last_error = ret;
    if (file) {
        result->file = *file;
    }
}

static esp_err_t verify_payload(const char *path,
                                const uint8_t *payload,
                                size_t payload_len,
                                const char *step,
                                d1l_export_canary_result_t *result,
                                uint32_t *chunks,
                                size_t *verified_bytes)
{
    uint8_t read_buf[D1L_RP2040_FILE_CHUNK_MAX] = {0};
    d1l_rp2040_file_result_t file = {0};
    size_t offset = 0;
    if (chunks) {
        *chunks = 0;
    }
    if (verified_bytes) {
        *verified_bytes = 0;
    }
    while (offset < payload_len) {
        const size_t chunk_len =
            (payload_len - offset) > D1L_RP2040_FILE_CHUNK_MAX ?
            D1L_RP2040_FILE_CHUNK_MAX : (payload_len - offset);
        memset(read_buf, 0, sizeof(read_buf));
        const esp_err_t ret = d1l_rp2040_bridge_file_read(path, (uint32_t)offset,
                                                          read_buf, chunk_len,
                                                          &file, 3000U);
        if (ret != ESP_OK || file.length != chunk_len ||
            memcmp(read_buf, payload + offset, chunk_len) != 0) {
            result_step(result, step, ret == ESP_OK ? ESP_FAIL : ret, &file);
            return result->last_error;
        }
        offset += chunk_len;
        if (chunks) {
            (*chunks)++;
        }
        if (verified_bytes) {
            *verified_bytes = offset;
        }
    }
    if (!file.eof && offset == payload_len) {
        result_step(result, step, ESP_FAIL, &file);
        return result->last_error;
    }
    return ESP_OK;
}

static esp_err_t write_payload_atomic(const uint8_t *payload,
                                      size_t payload_len,
                                      const d1l_storage_status_t *status,
                                      d1l_export_canary_result_t *result)
{
    if (!payload || payload_len == 0 || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!d1l_export_store_sd_ready(status)) {
        result_step(result, "preflight", ESP_ERR_NOT_SUPPORTED, NULL);
        return ESP_ERR_NOT_SUPPORTED;
    }

    result->bytes = payload_len;

    d1l_rp2040_file_result_t file = {0};
    esp_err_t ret = d1l_rp2040_bridge_file_delete(result->tmp_path, &file, 3000U);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND && strcmp(file.err, "not_found") != 0) {
        result_step(result, "cleanup_tmp", ret, &file);
        return ret;
    }

    size_t offset = 0;
    while (offset < payload_len) {
        const size_t chunk_len =
            (payload_len - offset) > D1L_RP2040_FILE_CHUNK_MAX ?
            D1L_RP2040_FILE_CHUNK_MAX : (payload_len - offset);
        ret = d1l_rp2040_bridge_file_write(result->tmp_path, (uint32_t)offset,
                                           payload + offset, chunk_len,
                                           offset == 0, &file, 3000U);
        if (ret != ESP_OK || file.length != chunk_len ||
            file.size != offset + chunk_len) {
            result_step(result, "write_tmp", ret == ESP_OK ? ESP_FAIL : ret, &file);
            return result->last_error;
        }
        offset += chunk_len;
        result->chunks_written++;
    }
    result->write_tmp = true;

    ret = verify_payload(result->tmp_path, payload, payload_len, "read_tmp", result,
                         &result->chunks_verified_tmp, &result->tmp_verified_bytes);
    if (ret != ESP_OK) {
        return ret;
    }
    result->read_tmp = true;

    ret = d1l_rp2040_bridge_file_rename(result->tmp_path, result->path, true,
                                        &file, 3000U);
    if (ret != ESP_OK) {
        result_step(result, "rename_final", ret, &file);
        return ret;
    }
    result->rename_replace = true;

    ret = d1l_rp2040_bridge_file_stat(result->path, &file, 3000U);
    if (ret != ESP_OK || !file.exists || file.is_directory || file.size != payload_len) {
        result_step(result, "stat_final", ret == ESP_OK ? ESP_FAIL : ret, &file);
        return result->last_error;
    }
    result->stat_final = true;

    ret = verify_payload(result->path, payload, payload_len, "read_final", result,
                         &result->chunks_verified_final, &result->final_verified_bytes);
    if (ret != ESP_OK) {
        return ret;
    }
    result->read_final = true;
    result_step(result, "ok", ESP_OK, &file);
    return ESP_OK;
}

bool d1l_export_store_token_valid(const char *token)
{
    if (!token || token[0] == '\0') {
        return false;
    }
    size_t len = 0;
    while (token[len] != '\0') {
        const unsigned char ch = (unsigned char)token[len];
        if (!(isalnum(ch) || ch == '-' || ch == '_' || ch == '.')) {
            return false;
        }
        len++;
        if (len > D1L_EXPORT_CANARY_TOKEN_MAX) {
            return false;
        }
    }
    return true;
}

bool d1l_export_store_sd_ready(const d1l_storage_status_t *status)
{
    return status &&
           d1l_storage_status_policy_allows_cached_io(
               status->bridge_status_refresh_failures) &&
           status->sd_present &&
           status->sd_mounted &&
           status->sd_data_root_ready &&
           status->rp2040_sd_protocol_supported &&
           status->file_ops_supported &&
           status->atomic_rename_supported &&
           status->file_line_max >= D1L_RP2040_FILE_LINE_MAX &&
           status->file_chunk_max >= D1L_RP2040_FILE_CHUNK_MAX &&
           status->path_max >= D1L_RP2040_FILE_PATH_MAX;
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
esp_err_t d1l_export_store_write_canary(const char *token,
                                        const d1l_storage_status_t *status,
                                        d1l_export_canary_result_t *out_result)
{
    if (!token || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }

    d1l_export_canary_result_t result = {
        .public_rf_tx = false,
        .formats_sd = false,
        .last_error = ESP_OK,
    };
    if (!d1l_export_store_token_valid(token)) {
        result_step(&result, "token", ESP_ERR_INVALID_ARG, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(result.token, sizeof(result.token), "%s", token);
    if (!result_path_set(&result, token)) {
        result_step(&result, "path", ESP_ERR_INVALID_SIZE, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t payload[D1L_RP2040_FILE_CHUNK_MAX];
    const int payload_len = snprintf((char *)payload, sizeof(payload),
                                     "{\"schema\":1,\"kind\":\"diagnostic_export_canary\","
                                     "\"token\":\"%s\",\"public_rf_tx\":false,"
                                     "\"formats_sd\":false}\n",
                                     token);
    if (payload_len <= 0 || (size_t)payload_len >= sizeof(payload)) {
        result_step(&result, "payload", ESP_ERR_INVALID_SIZE, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = write_payload_atomic(payload, (size_t)payload_len, status, &result);
    *out_result = result;
    return ret;
}
#endif

esp_err_t d1l_export_store_write_diagnostics(const char *token,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             const d1l_storage_status_t *status,
                                             d1l_export_canary_result_t *out_result)
{
    if (!token || !payload || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }

    d1l_export_canary_result_t result = {
        .public_rf_tx = false,
        .formats_sd = false,
        .last_error = ESP_OK,
    };
    if (!d1l_export_store_token_valid(token)) {
        result_step(&result, "token", ESP_ERR_INVALID_ARG, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len == 0 || payload_len > D1L_EXPORT_DIAGNOSTIC_PAYLOAD_MAX) {
        result_step(&result, "payload", ESP_ERR_INVALID_SIZE, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(result.token, sizeof(result.token), "%s", token);
    if (!diagnostics_path_set(&result, token)) {
        result_step(&result, "path", ESP_ERR_INVALID_SIZE, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = write_payload_atomic(payload, payload_len, status, &result);
    *out_result = result;
    return ret;
}

esp_err_t d1l_export_store_write_data(const char *token,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      const d1l_storage_status_t *status,
                                      d1l_export_canary_result_t *out_result)
{
    if (!token || !payload || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }

    d1l_export_canary_result_t result = {
        .public_rf_tx = false,
        .formats_sd = false,
        .last_error = ESP_OK,
    };
    if (!d1l_export_store_token_valid(token)) {
        result_step(&result, "token", ESP_ERR_INVALID_ARG, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len == 0 || payload_len > D1L_EXPORT_DATA_PAYLOAD_MAX) {
        result_step(&result, "payload", ESP_ERR_INVALID_SIZE, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(result.token, sizeof(result.token), "%s", token);
    if (!data_path_set(&result, token)) {
        result_step(&result, "path", ESP_ERR_INVALID_SIZE, NULL);
        *out_result = result;
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = write_payload_atomic(payload, payload_len, status, &result);
    *out_result = result;
    return ret;
}
