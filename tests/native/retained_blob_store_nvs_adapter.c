#include "storage/retained_blob_store.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nvs.h"

/*
 * Native tests use the in-memory NVS stub for the fallback journal and the
 * small store below for removable SD. The default backend is disabled, which
 * preserves the legacy native-test behavior until a test explicitly inserts
 * an SD generation.
 */
#define TEST_SD_BLOB_MAX (128U * 1024U)
#define TEST_SD_KEY_MAX 32U

typedef struct {
    bool valid;
    char key[TEST_SD_KEY_MAX];
    uint8_t bytes[TEST_SD_BLOB_MAX];
    size_t len;
    size_t read_call_count;
    size_t write_commit_count;
    size_t erase_commit_count;
} test_sd_blob_t;

typedef struct {
    bool armed;
    bool enabled;
    uint32_t generation;
} test_backend_change_t;

static d1l_retained_blob_store_backend_state_t
    s_backend[D1L_RETAINED_BLOB_STORE_COUNT];
static test_sd_blob_t s_sd_blobs[D1L_RETAINED_BLOB_STORE_COUNT];
static test_backend_change_t
    s_change_after_read[D1L_RETAINED_BLOB_STORE_COUNT];
static test_backend_change_t
    s_change_before_write[D1L_RETAINED_BLOB_STORE_COUNT];
static test_backend_change_t
    s_change_before_erase[D1L_RETAINED_BLOB_STORE_COUNT];
static esp_err_t s_fail_before_write[D1L_RETAINED_BLOB_STORE_COUNT];
static esp_err_t s_fail_after_write[D1L_RETAINED_BLOB_STORE_COUNT];

static bool valid_store_id(d1l_retained_blob_store_id_t store_id)
{
    return store_id >= D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES &&
           store_id < D1L_RETAINED_BLOB_STORE_COUNT;
}

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

static esp_err_t nvs_read(
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

static esp_err_t nvs_write(
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

static esp_err_t nvs_erase(
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

static void apply_backend_change(
    d1l_retained_blob_store_id_t store_id,
    test_backend_change_t *change)
{
    if (!valid_store_id(store_id) || !change || !change->armed) {
        return;
    }
    s_backend[store_id].enabled = change->enabled;
    s_backend[store_id].generation = change->generation;
    change->armed = false;
}

void d1l_test_retained_blob_store_reset(void)
{
    memset(s_backend, 0, sizeof(s_backend));
    memset(s_sd_blobs, 0, sizeof(s_sd_blobs));
    memset(s_change_after_read, 0, sizeof(s_change_after_read));
    memset(s_change_before_write, 0, sizeof(s_change_before_write));
    memset(s_change_before_erase, 0, sizeof(s_change_before_erase));
    memset(s_fail_before_write, 0, sizeof(s_fail_before_write));
    memset(s_fail_after_write, 0, sizeof(s_fail_after_write));
}

void d1l_test_retained_blob_store_set_backend(
    d1l_retained_blob_store_id_t store_id, bool enabled,
    uint32_t generation)
{
    if (!valid_store_id(store_id)) {
        return;
    }
    s_backend[store_id].enabled = enabled;
    s_backend[store_id].generation = generation;
}

bool d1l_test_retained_blob_store_seed_sd(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len)
{
    if (!valid_store_id(store_id) || !key || !src || len == 0U ||
        len > TEST_SD_BLOB_MAX || strlen(key) >= TEST_SD_KEY_MAX) {
        return false;
    }
    test_sd_blob_t *slot = &s_sd_blobs[store_id];
    const size_t write_count = slot->write_commit_count;
    const size_t erase_count = slot->erase_commit_count;
    memset(slot, 0, sizeof(*slot));
    slot->valid = true;
    memcpy(slot->key, key, strlen(key) + 1U);
    memcpy(slot->bytes, src, len);
    slot->len = len;
    slot->write_commit_count = write_count;
    slot->erase_commit_count = erase_count;
    return true;
}

size_t d1l_test_retained_blob_store_copy_sd(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t dst_size)
{
    if (!valid_store_id(store_id) || !key) {
        return 0U;
    }
    const test_sd_blob_t *slot = &s_sd_blobs[store_id];
    if (!slot->valid || strcmp(slot->key, key) != 0) {
        return 0U;
    }
    if (dst && dst_size >= slot->len) {
        memcpy(dst, slot->bytes, slot->len);
    }
    return slot->len;
}

size_t d1l_test_retained_blob_store_sd_write_commit_count(
    d1l_retained_blob_store_id_t store_id)
{
    return valid_store_id(store_id) ?
        s_sd_blobs[store_id].write_commit_count : 0U;
}

size_t d1l_test_retained_blob_store_sd_read_call_count(
    d1l_retained_blob_store_id_t store_id)
{
    return valid_store_id(store_id) ?
        s_sd_blobs[store_id].read_call_count : 0U;
}

size_t d1l_test_retained_blob_store_sd_erase_commit_count(
    d1l_retained_blob_store_id_t store_id)
{
    return valid_store_id(store_id) ?
        s_sd_blobs[store_id].erase_commit_count : 0U;
}

void d1l_test_retained_blob_store_change_after_next_sd_read(
    d1l_retained_blob_store_id_t store_id, bool enabled,
    uint32_t generation)
{
    if (valid_store_id(store_id)) {
        s_change_after_read[store_id] = (test_backend_change_t) {
            .armed = true,
            .enabled = enabled,
            .generation = generation,
        };
    }
}

void d1l_test_retained_blob_store_change_before_next_sd_write(
    d1l_retained_blob_store_id_t store_id, bool enabled,
    uint32_t generation)
{
    if (valid_store_id(store_id)) {
        s_change_before_write[store_id] = (test_backend_change_t) {
            .armed = true,
            .enabled = enabled,
            .generation = generation,
        };
    }
}

void d1l_test_retained_blob_store_change_before_next_sd_erase(
    d1l_retained_blob_store_id_t store_id, bool enabled,
    uint32_t generation)
{
    if (valid_store_id(store_id)) {
        s_change_before_erase[store_id] = (test_backend_change_t) {
            .armed = true,
            .enabled = enabled,
            .generation = generation,
        };
    }
}

void d1l_test_retained_blob_store_fail_next_sd_write(
    d1l_retained_blob_store_id_t store_id, esp_err_t error,
    bool after_commit)
{
    if (!valid_store_id(store_id)) {
        return;
    }
    if (after_commit) {
        s_fail_after_write[store_id] = error;
    } else {
        s_fail_before_write[store_id] = error;
    }
}

bool d1l_retained_blob_store_backend_state(
    d1l_retained_blob_store_id_t store_id,
    d1l_retained_blob_store_backend_state_t *out_state)
{
    if (!valid_store_id(store_id) || !out_state) {
        return false;
    }
    *out_state = s_backend[store_id];
    return true;
}

esp_err_t d1l_retained_blob_store_read_sd_primary(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    if (!valid_store_id(store_id) || !key || !dst || !len_inout) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_backend[store_id].enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    test_sd_blob_t *slot = &s_sd_blobs[store_id];
    slot->read_call_count++;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (slot->valid && strcmp(slot->key, key) == 0) {
        const size_t available = *len_inout;
        *len_inout = slot->len;
        if (available < slot->len) {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(dst, slot->bytes, slot->len);
            ret = ESP_OK;
        }
    }
    apply_backend_change(store_id, &s_change_after_read[store_id]);
    return ret;
}

esp_err_t d1l_retained_blob_store_read_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    return nvs_read(store_id, key, dst, len_inout);
}

esp_err_t d1l_retained_blob_store_write_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len, uint32_t expected_generation)
{
    if (!valid_store_id(store_id) || !key || !src || len == 0U ||
        len > TEST_SD_BLOB_MAX || strlen(key) >= TEST_SD_KEY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_backend[store_id].enabled ||
        s_backend[store_id].generation != expected_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    apply_backend_change(store_id, &s_change_before_write[store_id]);
    if (!s_backend[store_id].enabled ||
        s_backend[store_id].generation != expected_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_fail_before_write[store_id] != ESP_OK) {
        const esp_err_t failure = s_fail_before_write[store_id];
        s_fail_before_write[store_id] = ESP_OK;
        return failure;
    }
    if (store_id == D1L_RETAINED_BLOB_STORE_CONTACTS) {
        const esp_err_t nvs_ret = nvs_write(store_id, key, src, len);
        if (nvs_ret != ESP_OK) {
            return nvs_ret;
        }
    }
    test_sd_blob_t *slot = &s_sd_blobs[store_id];
    const size_t write_count = slot->write_commit_count;
    const size_t erase_count = slot->erase_commit_count;
    memset(slot, 0, sizeof(*slot));
    slot->valid = true;
    memcpy(slot->key, key, strlen(key) + 1U);
    memcpy(slot->bytes, src, len);
    slot->len = len;
    slot->write_commit_count = write_count + 1U;
    slot->erase_commit_count = erase_count;
    if (s_fail_after_write[store_id] != ESP_OK) {
        const esp_err_t failure = s_fail_after_write[store_id];
        s_fail_after_write[store_id] = ESP_OK;
        return failure;
    }
    return ESP_OK;
}

esp_err_t d1l_retained_blob_store_write_sd_primary(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len)
{
    if (!valid_store_id(store_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    return d1l_retained_blob_store_write_sd_primary_guarded(
        store_id, key, src, len, s_backend[store_id].generation);
}

esp_err_t d1l_retained_blob_store_write_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len)
{
    return nvs_write(store_id, key, src, len);
}

esp_err_t d1l_retained_blob_store_erase_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id, const char *key,
    uint32_t expected_generation)
{
    if (!valid_store_id(store_id) || !key) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_backend[store_id].enabled ||
        s_backend[store_id].generation != expected_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    apply_backend_change(store_id, &s_change_before_erase[store_id]);
    if (!s_backend[store_id].enabled ||
        s_backend[store_id].generation != expected_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    test_sd_blob_t *slot = &s_sd_blobs[store_id];
    if (slot->valid && strcmp(slot->key, key) == 0) {
        slot->valid = false;
        slot->len = 0U;
        slot->key[0] = '\0';
    }
    slot->erase_commit_count++;
    return ESP_OK;
}

esp_err_t d1l_retained_blob_store_erase_sd_primary(
    d1l_retained_blob_store_id_t store_id, const char *key)
{
    if (!valid_store_id(store_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    return d1l_retained_blob_store_erase_sd_primary_guarded(
        store_id, key, s_backend[store_id].generation);
}

esp_err_t d1l_retained_blob_store_erase_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key)
{
    return nvs_erase(store_id, key);
}

esp_err_t d1l_retained_blob_store_read(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    return nvs_read(store_id, key, dst, len_inout);
}

esp_err_t d1l_retained_blob_store_write(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len)
{
    return nvs_write(store_id, key, src, len);
}

esp_err_t d1l_retained_blob_store_erase(
    d1l_retained_blob_store_id_t store_id, const char *key)
{
    return nvs_erase(store_id, key);
}
