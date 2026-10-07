#include "contact_policy.h"

#include <stdatomic.h>
#include <string.h>
#include "app/settings_model.h"
#include "mesh/store_lock.h"
#include "nvs.h"

/* Separate, identity-bound preference: 1.8 can still roll back using its
 * unchanged settings envelope. A reset/new identity cannot inherit it. */
typedef struct {
    uint8_t owner[D1L_IDENTITY_PUBLIC_KEY_LEN];
    uint8_t roles, max_hops, manual, version;
} policy_record_t;

static atomic_uint s_policy = ATOMIC_VAR_INIT(0x80U); /* Manual-only until loaded. */
static d1l_store_lock_t s_lock = D1L_STORE_LOCK_INITIALIZER;

static bool valid(d1l_contact_policy_t policy)
{
    return (policy.roles & ~D1L_CONTACT_AUTOADD_ALL) == 0U && policy.max_hops <= 64U;
}

static void publish(d1l_contact_policy_t policy)
{
    atomic_store(&s_policy, policy.roles | (policy.manual ? 0x80U : 0U) |
        ((unsigned)policy.max_hops << 8));
}

d1l_contact_policy_t d1l_contact_policy_get(void)
{
    const unsigned value = atomic_load(&s_policy);
    return (d1l_contact_policy_t){.roles = value & D1L_CONTACT_AUTOADD_ALL,
        .max_hops = (uint8_t)(value >> 8), .manual = (value & 0x80U) != 0U};
}

esp_err_t d1l_contact_policy_init(void)
{
    publish((d1l_contact_policy_t){.manual = true});
    d1l_settings_t settings;
    esp_err_t ret = d1l_settings_public_snapshot(&settings);
    if (ret != ESP_OK || !settings.identity_ready) return ESP_ERR_INVALID_STATE;
    policy_record_t record = {0};
    size_t length = sizeof(record);
    nvs_handle_t handle = 0U;
    ret = nvs_open("d1l_ui", NVS_READONLY, &handle);
    if (ret == ESP_OK) {
        ret = nvs_get_blob(handle, "contact_policy", &record, &length);
        nvs_close(handle);
    }
    if (ret == ESP_ERR_NVS_NOT_FOUND ||
        (ret == ESP_OK && length == sizeof(record) &&
         memcmp(record.owner, settings.identity_public_key, sizeof(record.owner)) != 0)) {
        publish((d1l_contact_policy_t){.roles = D1L_CONTACT_AUTOADD_ALL});
        return ESP_OK;
    }
    if (ret != ESP_OK) return ret;
    const d1l_contact_policy_t policy = {record.roles, record.max_hops, record.manual != 0U};
    if (length != sizeof(record) || record.version != 1U || record.manual > 1U || !valid(policy)) {
        return ESP_ERR_INVALID_STATE;
    }
    publish(policy);
    return ESP_OK;
}

esp_err_t d1l_contact_policy_save(d1l_contact_policy_t policy)
{
    if (!valid(policy)) return ESP_ERR_INVALID_ARG;
    d1l_settings_t settings;
    if (d1l_settings_public_snapshot(&settings) != ESP_OK || !settings.identity_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    policy_record_t record = {.roles = policy.roles, .max_hops = policy.max_hops,
        .manual = policy.manual, .version = 1U};
    memcpy(record.owner, settings.identity_public_key, sizeof(record.owner));
    SemaphoreHandle_t lock = d1l_store_lock_handle(&s_lock);
    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    nvs_handle_t handle = 0U;
    esp_err_t ret = nvs_open("d1l_ui", NVS_READWRITE, &handle);
    if (ret == ESP_OK) ret = nvs_set_blob(handle, "contact_policy", &record, sizeof(record));
    if (ret == ESP_OK) ret = nvs_commit(handle);
    if (handle) nvs_close(handle);
    if (ret == ESP_OK) publish(policy);
    xSemaphoreGive(lock);
    return ret;
}

