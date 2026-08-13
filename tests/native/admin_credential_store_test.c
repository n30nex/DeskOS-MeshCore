#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mesh/admin_credential_store.h"
#include "mock_esp_nvs.h"

static const char *FINGERPRINT = "0123456789abcdef";

int main(void)
{
    char password[D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U] = {0};
    mock_nvs_reset();

    assert(!d1l_admin_credential_store_has(FINGERPRINT));
    assert(d1l_admin_credential_store_load(FINGERPRINT, password) ==
           ESP_ERR_NOT_FOUND);
    assert(d1l_admin_credential_store_save("bad", "secret") ==
           ESP_ERR_INVALID_ARG);
    assert(d1l_admin_credential_store_save(FINGERPRINT, "") ==
           ESP_ERR_INVALID_ARG);

    assert(d1l_admin_credential_store_save(FINGERPRINT, "first") == ESP_OK);
    assert(d1l_admin_credential_store_has(FINGERPRINT));
    assert(d1l_admin_credential_store_load(FINGERPRINT, password) == ESP_OK);
    assert(strcmp(password, "first") == 0);

    assert(d1l_admin_credential_store_save(FINGERPRINT, "second") == ESP_OK);
    memset(password, 0, sizeof(password));
    assert(d1l_admin_credential_store_load(FINGERPRINT, password) == ESP_OK);
    assert(strcmp(password, "second") == 0);

    mock_nvs_fail_next_commit(ESP_FAIL);
    assert(d1l_admin_credential_store_save(FINGERPRINT, "third") == ESP_FAIL);
    memset(password, 0, sizeof(password));
    assert(d1l_admin_credential_store_load(FINGERPRINT, password) == ESP_OK);
    assert(strcmp(password, "second") == 0);

    assert(d1l_admin_credential_store_forget(FINGERPRINT) == ESP_OK);
    assert(!d1l_admin_credential_store_has(FINGERPRINT));
    assert(d1l_admin_credential_store_forget(FINGERPRINT) ==
           ESP_ERR_NOT_FOUND);

    const uint32_t malformed = 0U;
    assert(mock_nvs_seed_blob(
        D1L_ADMIN_CREDENTIAL_NAMESPACE, D1L_ADMIN_CREDENTIAL_KEY,
        &malformed, sizeof(malformed)));
    assert(d1l_admin_credential_store_load(FINGERPRINT, password) ==
           ESP_ERR_INVALID_STATE);

    puts("admin_credential_store_test: ok");
    return 0;
}
