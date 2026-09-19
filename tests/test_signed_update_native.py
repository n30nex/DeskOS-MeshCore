"""Run the production update state machine with a replaceable SD/flash backend."""

from pathlib import Path
import re
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = re.search(r"(?:static\s+)?(?:esp_err_t|void|bool|uint32_t)\s+" + name + r"\([^)]*\)\s*\{", source)
    assert start, name
    end, depth = start.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start.start():end]


@pytest.fixture(scope="module")
def updater(tmp_path_factory):
    source = (ROOT / "main/update/update_manager.c").read_text()
    manifest_type = re.search(r"typedef struct \{.*?\} d1l_update_manifest_t;", source, re.S)
    assert manifest_type
    code = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "update/update_manager.h"
#include "update/update_signing_key.h"
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_INVALID_VERSION 0x10a
#define D1L_UPDATE_MANIFEST_MAX 768U
#define D1L_UPDATE_SIGNATURE_BYTES 64U
#define D1L_RP2040_FILE_CHUNK_MAX 192U
#define D1L_PARTITION_TABLE_SHA256 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define D1L_UPDATE_PROJECT_NAME "meshcore_deskos_d1l"
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 0x11
#define D1L_EVENT_LOG_LEVEL_INFO 1
#define D1L_EVENT_LOG_LEVEL_ERROR 2
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) (ms)
typedef struct { unsigned type, subtype, size; char label[17]; } esp_partition_t;
typedef struct { char project_name[32]; } esp_app_desc_t;
static const esp_partition_t partition = {0, 0x11, 4096, "ota_1"};
static d1l_update_status_t s_status;
static int s_lock;
static void *s_task = (void *)1;
static bool s_install_requested, s_cancel_requested, s_reboot_prepared;
static bool replace_image, read_failure, cancel_after_hash;
static unsigned writes, boot_selections, inspected;
static unsigned char flash_byte;
static jmp_buf task_exit;
static void taskYIELD(void) {}
static void vTaskDelay(unsigned ms) { (void)ms; longjmp(task_exit, 1); }
static const char *esp_err_to_name(int error) { (void)error; return "test error"; }
static void d1l_event_log_append(int level, const char *area, const char *event, const char *text) {
    (void)level; (void)area; (void)event; (void)text;
}
/* Digest fixtures deliberately use uniform bytes. Cryptography is covered by
 * the package tests; this backend checks which bytes get trusted and selected. */
typedef struct { unsigned char value; } mbedtls_sha256_context;
static void mbedtls_sha256_init(mbedtls_sha256_context *ctx) { ctx->value = 0; }
static void mbedtls_sha256_free(mbedtls_sha256_context *ctx) { (void)ctx; }
static int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int mode) { (void)mode; ctx->value = 0; return 0; }
static int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *bytes, size_t length) {
    if (length) ctx->value = bytes[0]; return 0;
}
static int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char *digest) {
    memset(digest, ctx->value, 32); return 0;
}
static esp_err_t esp_partition_read(const esp_partition_t *p, size_t offset, void *out, size_t length) {
    assert(p == &partition && offset + length <= 256);
    if (read_failure) return ESP_FAIL;
    memset(out, flash_byte, length); return ESP_OK;
}
static esp_err_t inspect_file(const char *path, uint32_t *size) {
    ++inspected;
    *size = strcmp(path, D1L_UPDATE_SIGNATURE_PATH) == 0 ? 64 :
        strcmp(path, D1L_UPDATE_IMAGE_PATH) == 0 ? 256 : 128;
    return ESP_OK;
}
static esp_err_t read_exact_file(const char *path, uint8_t *out, size_t size) {
    (void)path; memset(out, 0, size); return ESP_OK;
}
static uint32_t load_highest_sequence(void) { return 0; }
static bool d1l_ed25519_signature_s_is_canonical(const unsigned char *signature) { (void)signature; return true; }
static int ed25519_verify(const unsigned char *s, const unsigned char *m, size_t n, const unsigned char *k) {
    (void)s; (void)m; (void)n; (void)k; return 1;
}
static bool hex_to_bytes(const char *hex, uint8_t *out, size_t length) { (void)hex; memset(out, 0x11, length); return true; }
static esp_err_t hash_image(const char *path, uint32_t size, uint8_t *out) {
    (void)path; assert(size == 256); memset(out, 0x11, 32);
    if (cancel_after_hash) s_cancel_requested = true;
    return ESP_OK;
}
static const esp_partition_t *esp_ota_get_next_update_partition(const void *unused) { (void)unused; return &partition; }
static esp_err_t write_image(const char *path, uint32_t size, const esp_partition_t *p) {
    (void)path; assert(size == 256 && p == &partition); ++writes;
    flash_byte = replace_image ? 0x22 : 0x11; return ESP_OK;
}
static esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *out) {
    assert(p == &partition); strcpy(out->project_name, D1L_UPDATE_PROJECT_NAME); return ESP_OK;
}
static esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { assert(p == &partition); ++boot_selections; return ESP_OK; }
static void clear_pending(bool confirmed) { (void)confirmed; }
'''
    code += manifest_type.group(0) + r'''
static bool parse_manifest(const uint8_t *bytes, size_t size, d1l_update_manifest_t *out) {
    (void)bytes; (void)size; memset(out, 0, sizeof(*out));
    strcpy(out->partition_sha256, D1L_PARTITION_TABLE_SHA256);
    strcpy(out->version, "fixture"); strcpy(out->source_sha, "0123456789012345678901234567890123456789");
    out->image_size = 256; out->security_sequence = 123; return true;
}
static esp_err_t save_pending(const d1l_update_manifest_t *manifest) { (void)manifest; return ESP_OK; }
'''
    for name in ("secure_zero", "set_state", "cancel_requested"):
        code += function(source, name) + "\n"
    if "static esp_err_t hash_written_image(" in source:
        code += function(source, "hash_written_image") + "\n"
    for name in ("run_install", "update_task", "d1l_update_request_install", "d1l_update_cancel"):
        code += function(source, name) + "\n"
    code += r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "cancel_queued")) {
        assert(d1l_update_request_install() == ESP_OK);
        assert(d1l_update_cancel() == ESP_OK);
        if (!setjmp(task_exit)) update_task(NULL);
        assert(s_status.state == D1L_UPDATE_STATE_CANCELLED);
        assert(writes == 0 && boot_selections == 0);
    } else if (!strcmp(argv[1], "install_after_ready")) {
        assert(run_install() == ESP_OK && boot_selections == 1);
        assert(d1l_update_request_install() == ESP_ERR_INVALID_STATE);
    } else {
        replace_image = !strcmp(argv[1], "replaced_image");
        read_failure = !strcmp(argv[1], "readback_failure");
        cancel_after_hash = !strcmp(argv[1], "cancel_at_write_boundary");
        esp_err_t result = run_install();
        if (replace_image) assert(result == ESP_ERR_INVALID_CRC && boot_selections == 0);
        else if (read_failure) assert(result == ESP_FAIL && boot_selections == 0);
        else if (cancel_after_hash) assert(result == ESP_ERR_INVALID_STATE && writes == 0 && boot_selections == 0);
        else assert(result == ESP_OK && writes == 1 && boot_selections == 1 && s_status.reboot_required);
    }
    return 0;
}
'''
    directory = tmp_path_factory.mktemp("signed-update")
    program = directory / "update.c"
    program.write_text(code)
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required for update state-machine tests"
    binary = directory / "update"
    subprocess.run([compiler, "-std=c11", "-O2", "-I", str(ROOT / "main"),
                    "-I", str(ROOT / "tests/native/stubs"), str(program), "-o", str(binary)], check=True)
    return binary


@pytest.mark.parametrize("scenario", ["verified_image", "replaced_image", "readback_failure",
                                    "cancel_queued", "cancel_at_write_boundary", "install_after_ready"])
def test_update_selects_only_verified_flash_and_honours_cancellation(updater, scenario):
    subprocess.run([str(updater), scenario], check=True)


@pytest.fixture(scope="module")
def boot_result(tmp_path_factory):
    source = (ROOT / "main/update/update_manager.c").read_text()
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "update/update_manager.h"
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define D1L_UPDATE_NAMESPACE "update"
#define D1L_UPDATE_PROJECT_NAME "meshcore_deskos_d1l"
#define D1L_UPDATE_MIN_INTERNAL_HEAP_BYTES 16384
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 0x11
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define D1L_EVENT_LOG_LEVEL_INFO 1
#define D1L_EVENT_LOG_LEVEL_ERROR 2
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef unsigned nvs_handle_t;
typedef enum { ESP_OTA_IMG_UNDEFINED, ESP_OTA_IMG_VALID, ESP_OTA_IMG_PENDING_VERIFY } esp_ota_img_states_t;
typedef struct { unsigned type, subtype; char label[17]; } esp_partition_t;
typedef struct { char project_name[32]; } esp_app_desc_t;
typedef struct { bool pending; uint32_t sequence, highest; char result[16]; } receipt_t;
static receipt_t durable = {true, 123, 41, ""}, staged;
static d1l_update_status_t s_status = {.highest_security_sequence = 41};
static esp_partition_t partition = {0, 0x10, "ota_0"};
static esp_ota_img_states_t image_state = ESP_OTA_IMG_VALID;
static int s_lock, failure;
static unsigned commits;
static esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    (void)name; (void)mode;
    if (failure == 1) return ESP_FAIL;
    staged = durable; *handle = 1; return ESP_OK;
}
static void nvs_close(nvs_handle_t handle) { (void)handle; }
static esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *value) {
    (void)handle;
    if (failure == 2) return ESP_FAIL;
    if (!strcmp(key, "pending_seq")) {
        if (!staged.pending) return ESP_ERR_NVS_NOT_FOUND;
        *value = staged.sequence;
    } else { assert(!strcmp(key, "highest_seq")); *value = staged.highest; }
    return ESP_OK;
}
static esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value) {
    (void)handle; assert(!strcmp(key, "highest_seq"));
    if (failure == 3) return ESP_FAIL;
    staged.highest = value; return ESP_OK;
}
static esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
    (void)handle; assert(!strcmp(key, "last_result"));
    strcpy(staged.result, value); return ESP_OK;
}
static esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    (void)handle;
    if (failure == 4) return ESP_FAIL;
    if (!strcmp(key, "pending_seq")) staged.pending = false;
    return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t handle) {
    (void)handle; ++commits;
    if (failure == 5) return ESP_FAIL;
    durable = staged; return ESP_OK;
}
static const esp_partition_t *esp_ota_get_running_partition(void) { return &partition; }
static esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *state) {
    assert(p == &partition); *state = image_state; return ESP_OK;
}
static esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *out) {
    assert(p == &partition); strcpy(out->project_name, D1L_UPDATE_PROJECT_NAME); return ESP_OK;
}
static unsigned heap_caps_get_free_size(unsigned flags) { (void)flags; return 49152; }
static esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { image_state = ESP_OTA_IMG_VALID; return ESP_OK; }
static esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void) { assert(0); return ESP_FAIL; }
static void esp_restart(void) { assert(0); }
static void d1l_event_log_append(int level, const char *area, const char *event, const char *text) {
    (void)level; (void)area; (void)event; (void)text;
}
'''
    for name in ("set_state", "load_highest_sequence", "clear_pending", "d1l_update_boot_confirm"):
        code += function(source, name) + "\n"
    code += r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    bool rollback = !strcmp(argv[1], "rollback");
    bool no_pending = !strcmp(argv[1], "no_pending");
    if (no_pending) durable.pending = false;
    if (!rollback && !no_pending) {
        image_state = ESP_OTA_IMG_PENDING_VERIFY;
        partition.subtype = 0x11; strcpy(partition.label, "ota_1");
    }
    if (!strcmp(argv[1], "monotonic")) durable.highest = 456;
    if (!strcmp(argv[1], "open_failure")) failure = 1;
    if (!strcmp(argv[1], "read_failure")) failure = 2;
    if (!strcmp(argv[1], "write_failure")) failure = 3;
    if (!strcmp(argv[1], "erase_failure")) failure = 4;
    if (!strcmp(argv[1], "commit_failure")) failure = 5;
    esp_err_t ret = d1l_update_boot_confirm(ESP_OK);
    if (failure) {
        assert(ret == ESP_FAIL && s_status.state == D1L_UPDATE_STATE_ERROR);
        assert(s_status.last_error == ESP_FAIL && !s_status.running_image_confirmed);
        assert(durable.pending && durable.highest == 41);
        assert(s_status.highest_security_sequence == 41);
    } else if (no_pending) {
        assert(ret == ESP_OK && s_status.state == D1L_UPDATE_STATE_IDLE && commits == 0);
    } else if (rollback) {
        assert(ret == ESP_OK && s_status.state == D1L_UPDATE_STATE_ROLLED_BACK);
        assert(!durable.pending && !strcmp(durable.result, "rolled_back"));
        assert(s_status.highest_security_sequence == 41 && !s_status.running_image_confirmed);
    } else {
        assert(ret == ESP_OK && s_status.running_image_confirmed);
        assert(!durable.pending && !strcmp(durable.result, "confirmed"));
        assert(s_status.highest_security_sequence == durable.highest);
        assert(durable.highest == (!strcmp(argv[1], "monotonic") ? 456 : 123));
    }
    return 0;
}
'''
    directory = tmp_path_factory.mktemp("update-boot-result")
    program = directory / "boot.c"
    program.write_text(code)
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler
    binary = directory / "boot-result"
    subprocess.run([compiler, "-std=c11", "-O2", "-I", str(ROOT / "main"),
                    "-I", str(ROOT / "tests/native/stubs"), str(program), "-o", str(binary)], check=True)
    return binary


@pytest.mark.parametrize("scenario", ["rollback", "confirmed", "monotonic", "no_pending",
                                     "open_failure", "read_failure", "write_failure",
                                     "erase_failure", "commit_failure"])
def test_update_boot_result_matches_durable_receipt(boot_result, scenario):
    subprocess.run([str(boot_result), scenario], check=True)


def test_signed_package_destinations_match_the_bridge_root(tmp_path, monkeypatch):
    from scripts import package_release_d1l

    openssl = shutil.which("openssl")
    assert openssl, "OpenSSL is required by the signed-release packager"
    key = tmp_path / "fixture-key.pem"
    subprocess.run([openssl, "genpkey", "-algorithm", "ED25519", "-out", str(key)], check=True)
    public = subprocess.check_output([openssl, "pkey", "-in", str(key), "-pubout", "-outform", "DER"])
    monkeypatch.setattr(package_release_d1l, "UPDATE_SIGNING_PUBLIC_KEY_HEX", public[-32:].hex())
    package = tmp_path / "package"
    update = package / "update"
    update.mkdir(parents=True)
    (update / "d1l-update.bin").write_bytes(b"\xe9" + bytes(1023))
    bundle = package_release_d1l.write_signed_update_bundle(
        ROOT, package, {"path": "update/d1l-update.bin"}, "a" * 40, "1.8.0-rc.2", 123, key)
    bridge = (ROOT / "firmware/rp2040_sd_bridge/deskos_sd_bridge/deskos_sd_bridge.ino").read_text()
    header = (ROOT / "main/update/update_manager.h").read_text()
    data_root = re.search(r'DESKOS_ROOT = "([^"]+)"', bridge).group(1).lstrip("/")
    for kind in ("image", "manifest", "signature"):
        relative = re.search(r'#define D1L_UPDATE_' + kind.upper() + r'_PATH "([^"]+)"', header).group(1)
        assert bundle[kind]["sd_destination"] == f"{data_root}/{relative}"
