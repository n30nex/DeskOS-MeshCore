import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_quick_replies_persist_validate_and_preserve_failed_saves(tmp_path):
    harness = tmp_path / "replies.c"
    harness.write_text(r'''
#include <assert.h>
#include <string.h>
#include "ui/quick_replies.h"
#include "nvs.h"

static d1l_quick_replies_t stored, pending;
static bool present, fail_commit;
static size_t stored_size = sizeof(stored);
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) {
    assert(strcmp(ns, "d1l_ui") == 0);
    if (!present && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    *h = 1; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(h == 1); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *size) {
    assert(h == 1 && strcmp(key, "quick_replies") == 0);
    if (!present) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < stored_size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(data, stored, stored_size); *size = stored_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size) {
    assert(h == 1 && strcmp(key, "quick_replies") == 0 && size == sizeof(stored));
    memcpy(pending, data, size); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    assert(h == 1);
    if (fail_commit) return ESP_FAIL;
    memcpy(stored, pending, sizeof(stored)); present = true; return ESP_OK;
}
int main(void) {
    d1l_quick_replies_t replies;
    assert(d1l_quick_replies_load(replies) == ESP_OK);
    assert(strcmp(replies[1], "Yes") == 0 && !present);
    assert(d1l_quick_reply_save(1, "Cafe\xc3\xa9") == ESP_OK);
    assert(d1l_quick_replies_load(replies) == ESP_OK);
    assert(strcmp(replies[1], "Cafe\xc3\xa9") == 0 && strcmp(replies[2], "No") == 0);
    fail_commit = true;
    assert(d1l_quick_reply_save(1, "replacement") == ESP_FAIL);
    assert(d1l_quick_replies_load(replies) == ESP_OK);
    assert(strcmp(replies[1], "Cafe\xc3\xa9") == 0);
    fail_commit = false;
    assert(d1l_quick_reply_save(1, "") == ESP_OK);
    assert(d1l_quick_replies_load(replies) == ESP_OK && replies[1][0] == '\0');
    assert(d1l_quick_reply_save(6, "bad slot") == ESP_ERR_INVALID_ARG);
    assert(d1l_quick_reply_save(0, "\xff") == ESP_ERR_INVALID_ARG);
    assert(d1l_quick_reply_save(0, "\x01") == ESP_ERR_INVALID_ARG);
    char full[139]; memset(full, 'a', sizeof(full)); full[138] = '\0';
    assert(d1l_quick_reply_save(0, full) == ESP_ERR_INVALID_ARG);
    full[80] = '\0';
    assert(d1l_quick_reply_save(0, full) == ESP_OK);
    memset(full, 'a', 138); full[138] = '\0';
    assert(d1l_quick_reply_fits(full, ""));
    assert(!d1l_quick_reply_fits(full, "x"));
    full[136] = '\0';
    assert(d1l_quick_reply_fits(full, "\xc3\xa9"));
    assert(!d1l_quick_reply_fits(full, "\xe2\x9c\x93"));
    assert(d1l_quick_reply_fits("", "Received"));
    assert(!d1l_quick_reply_fits("\xff", "Received"));
    assert(!d1l_quick_reply_fits("draft", "\xff"));
    stored_size -= 1;
    assert(d1l_quick_replies_load(replies) == ESP_ERR_INVALID_SIZE);
    assert(d1l_quick_reply_save(0, "do not overwrite corruption") == ESP_ERR_INVALID_SIZE);
    stored_size += 1;
    memset(stored[0], 'x', sizeof(stored[0]));
    assert(d1l_quick_replies_load(replies) == ESP_ERR_INVALID_ARG);
    return 0;
}
''', encoding="utf-8")
    exe = tmp_path / "replies"
    subprocess.run([shutil.which("gcc") or "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "tests/native/stubs"), "-I", str(ROOT / "main"),
                    str(ROOT / "main/ui/quick_replies.c"), str(ROOT / "main/mesh/user_text.c"),
                    str(harness), "-o", str(exe)], check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
