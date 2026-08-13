#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "mesh/meshcore_admin_dispatch.h"
#include "mesh/node_store.h"

#define D1L_ADMIN_CREDENTIAL_NAMESPACE "d1l_admin_pw"
#define D1L_ADMIN_CREDENTIAL_KEY "credentials"

bool d1l_admin_credential_store_has(const char *fingerprint);
esp_err_t d1l_admin_credential_store_load(
    const char *fingerprint,
    char out_password[D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U]);
esp_err_t d1l_admin_credential_store_save(
    const char *fingerprint, const char *password);
esp_err_t d1l_admin_credential_store_forget(const char *fingerprint);

