#include "storage/retained_blob_store.h"

#include <string.h>

#include "nvs.h"

/*
 * Native tests use the in-memory NVS stub as a byte-addressable stand-in for
 * removable SD. Production contact and read-state code only calls the retained
 * blob-store API.
 */
static const char *test_namespace(d1l_retained_blob_store_id_t store_id)
{
    switch (store_id) {
    case D1L_RETAINED_BLOB_STORE_CONTACTS:
        return "d1l_contacts";
    case D1L_RETAINED_BLOB_STORE_READ_STATE:
        return "d1l_read";
    default:
        return NULL;
    }
}

esp_err_t d1l_retained_blob_store_read(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    const char *namespace_name = test_namespace(store_id);
    if (!namespace_name || !key || !dst || !len_inout) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(namespace_name, NVS_READONLY, &handle);
    if (ret == ESP_OK) {
        ret = nvs_get_blob(handle, key, dst, len_inout);
        nvs_close(handle);
    }
    return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : ret;
}

esp_err_t d1l_retained_blob_store_write(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len)
{
    const char *namespace_name = test_namespace(store_id);
    if (!namespace_name || !key || !src || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = nvs_set_blob(handle, key, src, len);
        if (ret == ESP_OK) {
            ret = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return ret;
}

esp_err_t d1l_retained_blob_store_erase(
    d1l_retained_blob_store_id_t store_id, const char *key)
{
    const char *namespace_name = test_namespace(store_id);
    if (!namespace_name || !key) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = nvs_erase_key(handle, key);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        } else if (ret == ESP_OK) {
            ret = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return ret;
}
