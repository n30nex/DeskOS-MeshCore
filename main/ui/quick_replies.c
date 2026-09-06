#include "quick_replies.h"

#include <string.h>
#include "mesh/user_text.h"
#include "nvs.h"

static const d1l_quick_replies_t defaults = {
    "Received, thank you.", "Yes", "No", "Please repeat that.",
    "I am on my way.", "I will reply shortly.",
};

bool d1l_quick_reply_fits(const char *draft, const char *reply)
{
    const d1l_user_text_info_t draft_info = d1l_user_text_validate(draft);
    const d1l_user_text_info_t reply_info = d1l_user_text_validate_bounded(
        reply, D1L_QUICK_REPLY_BYTES, true);
    return (draft_info.result == D1L_USER_TEXT_OK ||
            draft_info.result == D1L_USER_TEXT_EMPTY) &&
        reply_info.result == D1L_USER_TEXT_OK &&
        draft_info.byte_count + reply_info.byte_count <= D1L_USER_TEXT_MAX_BYTES;
}

esp_err_t d1l_quick_replies_load(d1l_quick_replies_t replies)
{
    if (!replies) return ESP_ERR_INVALID_ARG;
    memcpy(replies, defaults, sizeof(defaults));
    nvs_handle_t handle = 0U;
    esp_err_t result = nvs_open("d1l_ui", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    d1l_quick_replies_t loaded = {0};
    size_t length = sizeof(loaded);
    result = nvs_get_blob(handle, "quick_replies", loaded, &length);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    if (length != sizeof(loaded)) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0U; i < D1L_QUICK_REPLY_COUNT; ++i) {
        if (d1l_user_text_validate_bounded(loaded[i], sizeof(loaded[i]), true).result !=
            D1L_USER_TEXT_OK) return ESP_ERR_INVALID_ARG;
    }
    memcpy(replies, loaded, sizeof(loaded));
    return ESP_OK;
}

esp_err_t d1l_quick_reply_save(size_t index, const char *text)
{
    if (index >= D1L_QUICK_REPLY_COUNT ||
        d1l_user_text_validate_bounded(text, D1L_QUICK_REPLY_BYTES, true).result !=
            D1L_USER_TEXT_OK) return ESP_ERR_INVALID_ARG;
    d1l_quick_replies_t replies;
    esp_err_t result = d1l_quick_replies_load(replies);
    if (result != ESP_OK) return result;
    memset(replies[index], 0, sizeof(replies[index]));
    memcpy(replies[index], text, strlen(text));
    nvs_handle_t handle = 0U;
    result = nvs_open("d1l_ui", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, "quick_replies", replies, sizeof(replies));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}
