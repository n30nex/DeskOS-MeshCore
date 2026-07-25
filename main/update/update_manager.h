#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define D1L_UPDATE_MANIFEST_PATH "updates/d1l-update.manifest"
#define D1L_UPDATE_SIGNATURE_PATH "updates/d1l-update.sig"
#define D1L_UPDATE_IMAGE_PATH "updates/d1l-update.bin"
#define D1L_UPDATE_SOURCE_SHA_LEN 41U
#define D1L_UPDATE_VERSION_LEN 32U

typedef enum {
    D1L_UPDATE_STATE_IDLE = 0,
    D1L_UPDATE_STATE_INSPECTING,
    D1L_UPDATE_STATE_VERIFYING_SIGNATURE,
    D1L_UPDATE_STATE_VERIFYING_IMAGE,
    D1L_UPDATE_STATE_WRITING,
    D1L_UPDATE_STATE_FINALIZING,
    D1L_UPDATE_STATE_REBOOT_REQUIRED,
    D1L_UPDATE_STATE_CANCELLED,
    D1L_UPDATE_STATE_ROLLED_BACK,
    D1L_UPDATE_STATE_ERROR,
} d1l_update_state_t;

typedef struct {
    d1l_update_state_t state;
    bool initialized;
    bool install_requested;
    bool cancel_allowed;
    bool reboot_required;
    bool running_image_confirmed;
    bool rollback_enabled;
    uint8_t progress_percent;
    uint32_t image_size;
    uint32_t bytes_verified;
    uint32_t bytes_written;
    uint32_t security_sequence;
    uint32_t highest_security_sequence;
    char version[D1L_UPDATE_VERSION_LEN];
    char source_sha[D1L_UPDATE_SOURCE_SHA_LEN];
    char signer_key_id[32];
    char running_partition[17];
    char target_partition[17];
    esp_err_t last_error;
} d1l_update_status_t;

esp_err_t d1l_update_manager_init(void);
esp_err_t d1l_update_boot_confirm(esp_err_t nvs_status);
esp_err_t d1l_update_request_install(void);
esp_err_t d1l_update_cancel(void);
esp_err_t d1l_update_prepare_reboot(void);
void d1l_update_execute_prepared_reboot(void) __attribute__((noreturn));
esp_err_t d1l_update_reboot_to_installed(void);
void d1l_update_status(d1l_update_status_t *out_status);
const char *d1l_update_state_name(d1l_update_state_t state);

#ifdef __cplusplus
}
#endif
