#include "hal/rp2040_file_reply.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define D1L_RP2040_FILE_REPLY_PREFIX "DESKOS_SD_FILE"
#define D1L_RP2040_DATA64_MAX \
    (((D1L_RP2040_FILE_CHUNK_MAX + 2U) / 3U) * 4U)

static bool line_has_prefix(const char *line, const char *prefix)
{
    const size_t prefix_len = strlen(prefix);
    return line && strncmp(line, prefix, prefix_len) == 0 &&
           (line[prefix_len] == '\0' || line[prefix_len] == ' ');
}

static bool token_value_span(const char *line, const char *key,
                             const char **out_value, size_t *out_len)
{
    if (!line || !key || !out_value || !out_len) {
        return false;
    }
    const size_t key_len = strlen(key);
    const char *p = line;
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        const char *token = p;
        while (*p && *p != ' ') {
            ++p;
        }
        const size_t token_len = (size_t)(p - token);
        /* Equality is intentional: data= is the canonical zero-byte EOF
         * payload emitted by the RP2040 bridge. Typed non-data parsers still
         * reject an empty value through their own length requirements. */
        if (token_len >= key_len + 1U &&
            strncmp(token, key, key_len) == 0 &&
            token[key_len] == '=') {
            *out_value = token + key_len + 1U;
            *out_len = token_len - key_len - 1U;
            return true;
        }
    }
    return false;
}

static bool copy_token_value(const char *line, const char *key,
                             char *dest, size_t dest_size)
{
    const char *value = NULL;
    size_t value_len = 0U;
    if (!dest || dest_size == 0U ||
        !token_value_span(line, key, &value, &value_len) ||
        value_len + 1U > dest_size) {
        return false;
    }
    memcpy(dest, value, value_len);
    dest[value_len] = '\0';
    return true;
}

static bool parse_bool_token(const char *line, const char *key, bool *out_value)
{
    const char *value = NULL;
    size_t value_len = 0U;
    if (!out_value || !token_value_span(line, key, &value, &value_len) ||
        value_len != 1U || (value[0] != '0' && value[0] != '1')) {
        return false;
    }
    *out_value = value[0] == '1';
    return true;
}

static bool parse_u32_token(const char *line, const char *key,
                            uint32_t *out_value)
{
    const char *value = NULL;
    size_t value_len = 0U;
    if (!out_value || !token_value_span(line, key, &value, &value_len) ||
        value_len == 0U) {
        return false;
    }
    uint32_t parsed = 0U;
    for (size_t i = 0U; i < value_len; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
        const uint32_t digit = (uint32_t)(value[i] - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *out_value = parsed;
    return true;
}

static bool parse_hex_u32_token(const char *line, const char *key,
                                uint32_t *out_value)
{
    const char *value = NULL;
    size_t value_len = 0U;
    if (!out_value || !token_value_span(line, key, &value, &value_len) ||
        value_len != 8U) {
        return false;
    }
    uint32_t parsed = 0U;
    for (size_t i = 0U; i < value_len; ++i) {
        const char c = value[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            digit = (uint32_t)(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            digit = (uint32_t)(c - 'a' + 10);
        } else {
            return false;
        }
        parsed = (parsed << 4) | digit;
    }
    *out_value = parsed;
    return true;
}

static void parse_word_token(const char *line, const char *key,
                             char *dest, size_t dest_size)
{
    if (!copy_token_value(line, key, dest, dest_size) &&
        dest && dest_size > 0U) {
        dest[0] = '\0';
    }
}

static esp_err_t map_file_error(const char *err)
{
    if (!err || err[0] == '\0') {
        return ESP_FAIL;
    }
    if (strcmp(err, "no_card") == 0 || strcmp(err, "not_found") == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (strcmp(err, "not_ready") == 0 || strcmp(err, "exists") == 0 ||
        strcmp(err, "is_dir") == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(err, "bad_path") == 0 || strcmp(err, "bad_request") == 0 ||
        strcmp(err, "bad_value") == 0 || strcmp(err, "decode_failed") == 0 ||
        strcmp(err, "crc_mismatch") == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(err, "range") == 0 || strcmp(err, "too_large") == 0 ||
        strcmp(err, "line_too_long") == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (strcmp(err, "unsupported_op") == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (strcmp(err, "timeout") == 0) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_FAIL;
}

static uint32_t crc32_bytes(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0U; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (uint32_t)(-((int32_t)(crc & 1U)));
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return ~crc;
}

static int base64url_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return -1;
}

static bool base64url_decode(const char *input, uint8_t *out,
                             size_t out_size, size_t *out_len)
{
    if (!input || !out || !out_len) {
        return false;
    }
    const size_t input_len = strlen(input);
    if ((input_len % 4U) == 1U) {
        return false;
    }
    uint32_t value = 0U;
    int value_bits = -8;
    size_t used = 0U;
    for (size_t i = 0U; i < input_len; ++i) {
        const int part = base64url_value(input[i]);
        if (part < 0) {
            return false;
        }
        value = (value << 6) | (uint32_t)part;
        value_bits += 6;
        if (value_bits >= 0) {
            if (used >= out_size) {
                return false;
            }
            out[used++] = (uint8_t)((value >> value_bits) & 0xFFU);
            value_bits -= 8;
        }
    }
    *out_len = used;
    return true;
}

static esp_err_t bad_response(d1l_rp2040_file_result_t *result)
{
    result->last_error = ESP_FAIL;
    (void)snprintf(result->err, sizeof(result->err), "bad_response");
    (void)snprintf(result->note, sizeof(result->note), "bad_response");
    return ESP_FAIL;
}

static esp_err_t response_mismatch(d1l_rp2040_file_result_t *result)
{
    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }
    result->last_error = ESP_ERR_INVALID_STATE;
    (void)snprintf(result->err, sizeof(result->err), "response_mismatch");
    (void)snprintf(result->note, sizeof(result->note), "response_mismatch");
    return result->last_error;
}

esp_err_t d1l_rp2040_file_reply_parse(
    const char *line, uint16_t expected_id, const char *expected_op,
    uint8_t *out_data, size_t out_data_size,
    d1l_rp2040_file_result_t *result)
{
    if (!line || !expected_op || !result ||
        !line_has_prefix(line, D1L_RP2040_FILE_REPLY_PREFIX)) {
        return ESP_FAIL;
    }

    const bool bridge_ready = result->bridge_ready;
    memset(result, 0, sizeof(*result));
    result->bridge_ready = bridge_ready;
    result->protocol_supported = true;
    result->last_error = ESP_OK;
    (void)snprintf(result->op, sizeof(result->op), "unknown");

    uint32_t version = 0U;
    uint32_t request_id = 0U;
    if (!parse_u32_token(line, "v", &version) ||
        version != D1L_RP2040_FILE_PROTOCOL_VERSION ||
        !parse_u32_token(line, "id", &request_id) ||
        request_id != expected_id || request_id > UINT16_MAX ||
        !parse_bool_token(line, "ok", &result->ok)) {
        return bad_response(result);
    }
    result->request_id = (uint16_t)request_id;
    parse_word_token(line, "op", result->op, sizeof(result->op));
    parse_word_token(line, "err", result->err, sizeof(result->err));
    parse_word_token(line, "note", result->note, sizeof(result->note));
    if (strcmp(result->op, expected_op) != 0) {
        result->last_error = ESP_FAIL;
        (void)snprintf(result->err, sizeof(result->err), "op_mismatch");
        (void)snprintf(result->note, sizeof(result->note), "op_mismatch");
        return ESP_FAIL;
    }
    if (!result->ok) {
        result->last_error = map_file_error(result->err);
        return result->last_error;
    }

    if (strcmp(expected_op, "stat") == 0) {
        bool exists = false;
        uint32_t size = 0U;
        result->exists = exists;
        char kind[8] = {0};
        if (!parse_bool_token(line, "exists", &exists) ||
            !copy_token_value(line, "kind", kind, sizeof(kind)) ||
            !parse_u32_token(line, "size", &size) ||
            (exists && strcmp(kind, "file") != 0 &&
             strcmp(kind, "dir") != 0) ||
            (!exists && (strcmp(kind, "none") != 0 || size != 0U))) {
            return bad_response(result);
        }
        result->exists = exists;
        result->is_directory = strcmp(kind, "dir") == 0;
        result->size = size;
    } else if (strcmp(expected_op, "read") == 0) {
        if (!parse_u32_token(line, "off", &result->offset) ||
            !parse_u32_token(line, "len", &result->length) ||
            !parse_bool_token(line, "eof", &result->eof) ||
            result->length > out_data_size ||
            result->length > D1L_RP2040_FILE_CHUNK_MAX || !out_data) {
            result->last_error = ESP_ERR_INVALID_SIZE;
            return result->last_error;
        }
        char encoded[D1L_RP2040_DATA64_MAX + 1U];
        if (!copy_token_value(line, "data", encoded, sizeof(encoded)) ||
            !parse_hex_u32_token(line, "crc", &result->crc32)) {
            return bad_response(result);
        }
        size_t decoded_len = 0U;
        if (!base64url_decode(encoded, out_data, out_data_size, &decoded_len) ||
            decoded_len != result->length ||
            crc32_bytes(out_data, decoded_len) != result->crc32) {
            result->last_error = ESP_FAIL;
            return result->last_error;
        }
    } else if (strcmp(expected_op, "create") == 0 ||
               strcmp(expected_op, "write") == 0 ||
               strcmp(expected_op, "append") == 0) {
        if (!parse_u32_token(line, "off", &result->offset) ||
            !parse_u32_token(line, "len", &result->length) ||
            !parse_u32_token(line, "size", &result->size)) {
            return bad_response(result);
        }
    }

    result->last_error = ESP_OK;
    return ESP_OK;
}

esp_err_t d1l_rp2040_file_reply_bind_read(
    d1l_rp2040_file_result_t *result, uint32_t requested_offset,
    size_t requested_max_len)
{
    if (!result || requested_max_len > D1L_RP2040_FILE_CHUNK_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return result->offset == requested_offset &&
           result->length <= (uint32_t)requested_max_len ?
           ESP_OK : response_mismatch(result);
}

esp_err_t d1l_rp2040_file_reply_bind_write(
    d1l_rp2040_file_result_t *result, uint32_t requested_offset,
    size_t requested_len)
{
    if (!result || requested_len > D1L_RP2040_FILE_CHUNK_MAX ||
        requested_offset > UINT32_MAX - (uint32_t)requested_len) {
        return ESP_ERR_INVALID_ARG;
    }
    return result->offset == requested_offset &&
           result->length == (uint32_t)requested_len &&
           result->size == requested_offset + (uint32_t)requested_len ?
           ESP_OK : response_mismatch(result);
}

esp_err_t d1l_rp2040_file_reply_bind_append(
    d1l_rp2040_file_result_t *result, size_t requested_len)
{
    if (!result || requested_len > D1L_RP2040_FILE_CHUNK_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return result->length == (uint32_t)requested_len &&
           result->size >= result->length &&
           result->offset == result->size - result->length ?
           ESP_OK : response_mismatch(result);
}
