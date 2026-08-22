#include "admin_credential_store.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "mesh/contact_store.h"
#include "mesh/store_lock.h"
#include "nvs.h"

#define D1L_ADMIN_CREDENTIAL_MAGIC UINT32_C(0x41445057)
#define D1L_ADMIN_CREDENTIAL_VERSION UINT32_C(1)
#define D1L_ADMIN_CREDENTIAL_CAPACITY 16U

typedef struct {
    bool valid;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char password[D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U];
    uint32_t last_used;
} d1l_admin_credential_record_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t next_use;
    d1l_admin_credential_record_t records[D1L_ADMIN_CREDENTIAL_CAPACITY];
} d1l_admin_credential_blob_t;

static d1l_store_lock_t s_lock = D1L_STORE_LOCK_INITIALIZER;

static void secure_zero(void *value, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)value;
    while (bytes && size > 0U) {
        *bytes++ = 0U;
        size--;
    }
}

static bool fingerprint_valid(const char *fingerprint)
{
    if (!fingerprint ||
        strnlen(fingerprint, D1L_NODE_FINGERPRINT_LEN) !=
            D1L_NODE_FINGERPRINT_LEN - 1U) {
        return false;
    }
    for (size_t i = 0U; i < D1L_NODE_FINGERPRINT_LEN - 1U; ++i) {
        if (!isxdigit((unsigned char)fingerprint[i])) {
            return false;
        }
    }
    return true;
}

static bool password_valid(const char *password)
{
    return password &&
        strnlen(password, D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U) <=
            D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES;
}

static void blob_defaults(d1l_admin_credential_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->magic = D1L_ADMIN_CREDENTIAL_MAGIC;
    blob->version = D1L_ADMIN_CREDENTIAL_VERSION;
    blob->next_use = 1U;
}

static esp_err_t blob_load(nvs_handle_t handle,
                           d1l_admin_credential_blob_t *blob)
{
    if (!blob) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t size = sizeof(*blob);
    esp_err_t ret = nvs_get_blob(
        handle, D1L_ADMIN_CREDENTIAL_KEY, blob, &size);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        blob_defaults(blob);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (size != sizeof(*blob) ||
        blob->magic != D1L_ADMIN_CREDENTIAL_MAGIC ||
        blob->version != D1L_ADMIN_CREDENTIAL_VERSION ||
        blob->next_use == 0U) {
        secure_zero(blob, sizeof(*blob));
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0U; i < D1L_ADMIN_CREDENTIAL_CAPACITY; ++i) {
        const d1l_admin_credential_record_t *record = &blob->records[i];
        if (record->valid &&
            (!fingerprint_valid(record->fingerprint) ||
             !password_valid(record->password) ||
             record->password[0] == '\0')) {
            secure_zero(blob, sizeof(*blob));
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

static esp_err_t blob_commit(nvs_handle_t handle,
                             const d1l_admin_credential_blob_t *blob)
{
    esp_err_t ret = nvs_set_blob(
        handle, D1L_ADMIN_CREDENTIAL_KEY, blob, sizeof(*blob));
    return ret == ESP_OK ? nvs_commit(handle) : ret;
}

static int record_index(const d1l_admin_credential_blob_t *blob,
                        const char *fingerprint)
{
    for (size_t i = 0U; i < D1L_ADMIN_CREDENTIAL_CAPACITY; ++i) {
        if (blob->records[i].valid &&
            strcmp(blob->records[i].fingerprint, fingerprint) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static size_t replacement_index(const d1l_admin_credential_blob_t *blob)
{
    size_t oldest = 0U;
    for (size_t i = 0U; i < D1L_ADMIN_CREDENTIAL_CAPACITY; ++i) {
        if (!blob->records[i].valid) {
            return i;
        }
        if (blob->records[i].last_used < blob->records[oldest].last_used) {
            oldest = i;
        }
    }
    return oldest;
}

static esp_err_t open_store(nvs_open_mode_t mode, nvs_handle_t *out_handle)
{
    return nvs_open(D1L_ADMIN_CREDENTIAL_NAMESPACE, mode, out_handle);
}

bool d1l_admin_credential_store_has(const char *fingerprint)
{
    char password[D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U] = {0};
    const bool found = d1l_admin_credential_store_load(
        fingerprint, password) == ESP_OK;
    secure_zero(password, sizeof(password));
    return found;
}

esp_err_t d1l_admin_credential_store_load(
    const char *fingerprint,
    char out_password[D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U])
{
    if (!fingerprint_valid(fingerprint) || !out_password) {
        return ESP_ERR_INVALID_ARG;
    }
    out_password[0] = '\0';
    d1l_store_lock_take(&s_lock);
    nvs_handle_t handle = 0U;
    d1l_admin_credential_blob_t blob = {0};
    esp_err_t ret = open_store(NVS_READONLY, &handle);
    if (ret == ESP_OK) {
        ret = blob_load(handle, &blob);
    }
    if (ret == ESP_OK) {
        const int index = record_index(&blob, fingerprint);
        if (index < 0) {
            ret = ESP_ERR_NOT_FOUND;
        } else {
            memcpy(out_password, blob.records[index].password,
                   sizeof(blob.records[index].password));
        }
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_ERR_NOT_FOUND;
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    secure_zero(&blob, sizeof(blob));
    d1l_store_lock_give(&s_lock);
    return ret;
}

esp_err_t d1l_admin_credential_store_save(
    const char *fingerprint, const char *password)
{
    if (!fingerprint_valid(fingerprint) || !password_valid(password) ||
        password[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_store_lock_take(&s_lock);
    nvs_handle_t handle = 0U;
    d1l_admin_credential_blob_t blob = {0};
    esp_err_t ret = open_store(NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = blob_load(handle, &blob);
    }
    if (ret == ESP_OK) {
        int index = record_index(&blob, fingerprint);
        if (index < 0) {
            index = (int)replacement_index(&blob);
        }
        d1l_admin_credential_record_t *record = &blob.records[index];
        secure_zero(record, sizeof(*record));
        record->valid = true;
        memcpy(record->fingerprint, fingerprint,
               D1L_NODE_FINGERPRINT_LEN);
        const size_t password_len = strlen(password);
        memcpy(record->password, password, password_len + 1U);
        record->last_used = blob.next_use++;
        if (blob.next_use == 0U) {
            for (size_t i = 0U; i < D1L_ADMIN_CREDENTIAL_CAPACITY; ++i) {
                blob.records[i].last_used = blob.records[i].valid ?
                    (uint32_t)(i + 1U) : 0U;
            }
            blob.next_use = D1L_ADMIN_CREDENTIAL_CAPACITY + 1U;
        }
        ret = blob_commit(handle, &blob);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    secure_zero(&blob, sizeof(blob));
    d1l_store_lock_give(&s_lock);
    return ret;
}

esp_err_t d1l_admin_credential_store_forget(const char *fingerprint)
{
    if (!fingerprint_valid(fingerprint)) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_store_lock_take(&s_lock);
    nvs_handle_t handle = 0U;
    d1l_admin_credential_blob_t blob = {0};
    esp_err_t ret = open_store(NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = blob_load(handle, &blob);
    }
    if (ret == ESP_OK) {
        const int index = record_index(&blob, fingerprint);
        if (index < 0) {
            ret = ESP_ERR_NOT_FOUND;
        } else {
            secure_zero(&blob.records[index], sizeof(blob.records[index]));
            ret = blob_commit(handle, &blob);
        }
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    secure_zero(&blob, sizeof(blob));
    d1l_store_lock_give(&s_lock);
    return ret;
}
